# APUMULT — the apumult study (internal reference multiplier) against ecalc

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed.

| item | title |
|---|---|
| A37CMP | apumult a37v1 spec against ecalc, item by item (139 rows) |
| apumult_summary | the apumult doc's own executive summary (claims, later checked by F1/F2) |
| apumult_F1 | categories A (SSD spill) and D (trim), ranked port list, architecture note, against ntt/ |
| apumult_F2 | categories B (madvise), C (in-place), E (instrumentation), F (kernel variants) against ntt/ |
| apumult_M | disk and APU memory measurements (aac6) |
| apumult_R | research: spilling to SSD and releasing memory on MI300A |
| AP34 | the later apumult optimization list (items A1-A34) against ecalc `main` 4b5bb14 (2026-10-23) |

## Topic summary

apumult is a host-resident reference multiplier/e pipeline (spec "a37v1") that reached 2.35e11 digits per node (475 GB, SSD spill, madvise) but is 3.2-3.7x slower per digit than ecalc at 4e10 (234.8 s against 63.5-73.1 s (m)). Its doc claimed that porting its spill/madvise stack would lift ecalc's 576-node target from 4.0e13 to about 1.3e14 digits. The study checked every apumult technique against ecalc `main` of that date and measured the disk and memory behaviour of the APU. Conclusions that still hold:

- **ecalc already has (or beats) nearly every a37v1 technique**: same four RNS primes, batch tier with all primes on the owning APU, fused pointwise/L^-1/Garner+carry, no bit reversal, correction-form anchored Newton, band-cut products, view-based arena. ecalc's 2^31 forward NTT is 99-100 ms (m, `archive:results_merged_2026-10-24/KERNELS.md §K13b`) against 144 ms; one-node 4e10 is 63.5 +- 1.5 s (m) against 80.6 s. Of 139 rows about 80 are the same or equivalent and 14 ecalc is ahead.
- **The "3.4x digits" headline does not transfer.** Memory is device-resident with one arena per APU mapped at init and sized by `dm_layout`; `db_free` returns bytes only to the pool, there is no madvise on hipMalloc memory (EINVAL on a PFNMAP VMA), and the node peak is a mapped capacity. A buffer lowers the ceiling only by shrinking `dm_layout`'s formula. Modelled one-node ceilings: 1.30e11 now, 1.68e11 with a P spill plus tail policy (V2), 1.70e11 with V3 (+Q spill, late r2) at the 2^31 cap (2^30: 1.44 -> 1.79 / 1.88e11): +30 %, not 3.4x; apumult's 2.02 B/digit is below ecalc's V3 floor of about 2.3 B/digit. At 576 nodes spilling buys only +5 % (the tree top level, 294 GB, binds next); the built chunking switch (`MN_T_CHUNK_MB=1024`, off) is cheaper (5.32e13 mod). The apumult doc's claims about ntt/ were mostly stale or wrong (list under apumult_F1).
- **What was adopted from the study (current code, see `ecalc/README.md`)**: `DM_TIGHT` (tight reciprocal reservation, per-pair release in the device tier's top levels; was F2 item 1, "E2"), `DM_TAIL_DEAD` (tail re-laid over dead inputs, F1's tail policy), `DB_POOL_VMM` (HIP VMM arena; also removes the contiguity/fragmentation failure), `BS_SEED_FILL`, `BS_BATCH_PIPE` (default 1 since D20; was A37CMP R5, `results/KERNELS.md §N3`). Not adopted: any SSD spill of P/Q/mu (only the checkpoint writer `ECALC_CKPT_TOP`, off by default), the OOM-killing watchdog, madvise, Karatsuba over the device grid (idea only, a time-for-memory trade), FWD_FUSE, "P20" 3-prime pack, base 10^19.
- **Rejected on principle**: skipping the remainder check (T1 residue verification is the only verifier at 3.7e13 digits), Stirling-balanced leaves (a node's term range spans < 3 %), global base 10^19, XNACK=1 (2x slower, RESULTS §55), CPU school tier, arenas on two dies.
- **Disk/APU facts (apumult_M, apumult_R)** that any future spill needs: the page cache lives in the same HBM; use O_DIRECT through a pinned hipHostMalloc bounce ring (O_DIRECT from a hipMalloc pointer fails with EFAULT); `hipMemGetInfo` is useless as a free gauge (use /proc/meminfo); overcommitting hipMalloc OOM-kills the process; madvise(DONTNEED) on host memory frees HBM for hipMalloc immediately; true out-of-core multiplication is hopeless (SSD/HBM bandwidth 1/2000-1/7000 against y-cruncher's needed 1/4).
- **Superseded/stale items**: the apumult summary's table of ecalc numbers (7e10 d_max, 73.1 s, 6.3 B/digit, 121.5 GB pool) is superseded by the F1 section 7 check; F2's per-doubling "used-at-peak" fractions were assumed and are covered by the later `DM_TIGHT` measurements (results/DIVISION.md); A37CMP R1 (GPU seed) is an open candidate, not adopted (check `archive:results_merged_2026-10-24/INDEX.md` for the seed items before starting it).

## A37CMP: apumult a37v1 spec against ecalc, item by item

Read-only comparison of the a37v1 spec against ecalc `main` @ 6e220db. Sources include `results/TIME_MODEL.md §S19B`, `archive:results_merged_2026-10-24/KERNELS.md §K13b`, `results/TARGET.md §TGTBENCH2`. The spec has no §17; §5/§6 are one line each.

**Five-sentence comparison.**
1. ecalc has almost every a37v1 technique, often better (identical primes `modarith.h:76`, per-APU batch tier, fused pointwise/L^-1/Garner+carry+P-add, anchored correction-form Newton, band-cut/mid/high products, view-based ping-pong arena, seed overlapped with the mapping).
2. ecalc's NTT is faster: 2^31 forward 99-100 ms (m, K13b) against 144 ms; modmul 1.31-1.34 Tmm/s (m, RESULTS §19/§31) against 0.94 in-NTT; register-blocked b16 and b1 (1.2-1.33x), bottom-up plans, 3*2^k lengths. a37v1's batch tier (21.0 s at 4e10, ntt 73 %, crt 23 %) is not better than ecalc's (scatter 1.1 + ntt 15.7 + crt 2.6 + merge 0.8 s at 4e10 (m)).
3. ecalc one-node 4e10 is 63.5 +- 1.5 s (m) against 80.6 s. At 10-576 nodes the wall is the fabric (S19B: exchange 79 % of the modelled 576 wall), so single-node kernel ideas touch only init + leaf (about 61 s of 465 s at 10 nodes, 61 of 222-266 s at 576 (mod)).
4. The most useful new fact is the seed: a37v1's GPU seed kernel does 4.35e9 terms in 6 s (spec) against a CPU seed of 25.9 s; ecalc dropped GPU seeds (TASKS 6.4 / E8) because aac7 mapping masked them. On the target mapping is 7x faster (MAP_RATE 0.010 s/GB (m), TGTBENCH2) and the model says seeds bind (init 20.6 + seed wait 9.3 = 29.9 s at 576, S18TGT). a37v1's GPU-seed pitfalls (copyout +9 s, pregrow exposed +10 s) do not apply to ecalc.
5. Two big a37v1 choices rejected: skipping the remainder check and Stirling-balanced leaves.

**Row verdicts (139 rows; only non-trivial ones listed, all others SAME / EQUIV / N/A).**
- Algorithm and representation (rows 1-15): same Taylor/binary splitting; division DIFF (DKM two halves, half-length reciprocal, -25.7 s at 1e11 (m, RESULTS §92)); verification stronger (T1 on 8 primes + T2). Base 10^19 (row 6-7): 3-prime products would allow +5.6 % digits per point (P20 pack, batch levels L <= 2^22, <= 1.5-2 s at 6.4e10 (mod)), but whole-code change; not recommended. ecalc has 3*2^44 | p-1 (superset), 3 primes up to 5.84e10 coefficient terms, P24 (24 digits/pt at 4 primes).
- Flow/arena (16-36): A never formed (10dP: S=P+Q in place); `BS_SEED_FILL=128` picks the span S (about 229-239) so the last span has <= 128 limbs (bs -21 s with BI_MUL1_FAST, RESULTS §86); dm/tree need binds the arena, not bs; four per-APU region arenas (remote HBM read 93 GB/s against 3.8 TB/s local (m)); VMM arena mapped in background (KFD serializes); school tier absent (loss 42-59 s (m), RESULTS §58); P,Q stay on device (no copyout). Open: row 28 seed stream handoff (Q2), row 29 base-2^64 sub-range seed (ecalc's decimal span step 2.6 ns/limb against binary 0.82; a37v1's leaf costs 201 us per 352 terms, so not evidence it beats ecalc).
- Tiers/NTT (37-68): batch tier SAME. Gaps: row 42 batch pipe (one stream per die, a host sync per tile; <= about 2 s at 6.4e10 (mod); later built as `BS_BATCH_PIPE`), row 40 pageable `hd[]` descriptors (<= 1 s), row 50 FWD_FUSE absent (about 10-15 % of the first forward at 2^31 (mod); <= 1 s at 576), row 58 twiddle seed (check once, Q4), row 63 no NOP-modmul breakdown (Q4), row 66 FMA contraction not confirmed in ISA (Q5). ecalc ahead: pass plan {b1 on 2^12 + 3 b16}, register-blocked b16/b1, modmul `NTT_MODMUL=1` (rint == floor in ecalc: 1328 vs 1331 Gmm/s (m)), CRT 2.6 s against about 4.8 s.
- Newton (69-79): all SAME/EQUIV except row 77 skip remainder check (rejected, R1) and row 79 early reciprocal chain: ecalc's chain to 34126 limbs takes 2.28 s minimum, 6.8-22.2 s typical (15 runs, mean 12.4 s (m, S19B §2)).
- Output/memory (80-83): row 80 `fmt18` peels 18 digits with %10 and /10 (`DC_FMT_MN` 0.061 s per 1e9 digits (m) = 5.8 ns/digit-core; S19B `dc` 2.8-4.1 s at 6.44e10); a37v1 formats 4e10 in 0.5 s; unsure whether `dc` is formatting, D2H or T1 residues. Memory 363.5 GB device / 405.6 GB node at 6.44e10 (mod, S18TGT), 5.6 B/digit.
- Flags (84-118): all analogues listed; `HSA_XNACK=1` is the opposite of ecalc (XNACK off; 1e10 80 -> 159 s with it (m, RESULTS §55)); `NTT_REGBLOCK=0` is a different technique under the same name.
- Pitfalls (121-128): SDMA + migratable SVM hang N/A with XNACK off (TASKS B7/H5 would be a risk if combined); hipHostRegister of fresh anon-mmap + concurrent GPU write gave 1/18 corruption in a37v1, ecalc pretouches (`mem.c`, `comm_ofi.c`) but **not verified** for `comm_shmem.c:318` (`S.pool` from `shmem_malloc`); `kill -KILL` of large GPU jobs poisons KFD: `rundriver.sh` uses SIGTERM + 60 s grace, `a14_soak.sh` and `g13d_hang.sh` still kill -KILL.
- Tests/"don't" items (129-139): ecalc stronger; GPU-seed warning does not transfer.

**Ranked shortlist of new task candidates.**

| rank | name | benefit | effort / risk |
|---|---|---|---|
| R1 | revive the GPU seed kernel behind a switch (one thread/wave per 229-term span, into region arenas) | -7...-12 s at 576 (mod; seed 22 s (mod, from SC 31.75 s at 9.17e10) -> about 10 s), about 0...-4 s at 10 nodes (mapping binds on aac7) | L (2-4 d), medium; exact integers so byte-identity checkable |
| R2 | microbench b64 sub-range seed against `bi_span_step` (CPU only) | decides R1 against a CPU rewrite; seed 22 -> 14-18 s (mod, low confidence) | S (0.5 d), none |
| R3 | pre-touch the SHMEM pool before `hipHostRegister` | removes a hypothetical rare corruption/hang (unquantified) | S (1 h), low |
| R4 | profile `dc`; table-based `fmt18` if formatting | <= about 3 s per node (mod) | S (2 h), bit-identical |
| R5 | batch tile pipeline (two streams, pinned descriptors) | <= about 2 s per node (mod) | M, low |
| R6 | reciprocal chain floor (2.28 s minimum) | <= about 2 s | S then M |
| R7 | FWD_FUSE for B-strategy top levels | <= 1 s (mod) | M, medium |
| R8 | "P20" 3-prime pack | <= 1.5-2 s (mod) | L, medium |
| R9 | SIGTERM + grace in soak scripts | avoids a poisoned node | S, none |

**Rejected (one line each).** R1/#77 skip remainder check / SKIP_XQ (T1 needs R; model gain at most 0.4u = 5-18 s at 10 nodes, low confidence); #20 Stirling-balanced leaves; #6 global base 10^19; #85 XNACK=1; #24 arenas on two dies (locality 3.8 TB/s against 93 GB/s (m)); #32 CPU school tier for level 0 (RESULTS §58 net loss 42-59 s). Equal or ahead already: modmul variant, 4-step NTT, stride-17 tile, PW/L^-1 fusion, Newton details, batch tier structure, ping-pong arena, A never formed.

**Open questions (measurement needed).** Q1 does the seed (about 22 s (mod) at 6.44e10) bind init at 576 nodes (only data: SC rehearsal at 9.17e10, seed ends 31.75 s vs mapping 30.62 s, aac6; settle with a single-node `ECALC_INIT_TL=1` run on the target); Q2 seed stream handoff stalls; Q3 XNACK retest (only if pinning cost shows); Q4 pass-level NTT breakdown (HBM floor 51 ms (a37v1) against ecalc 99 ms); Q5 FMA contraction in `k_b16r` ISA; Q6 SHMEM pool pre-touch; Q7 what `dc` is; Q8 reciprocal chain floor and wait; Q9 fresh by-phase batch breakdown at 6.44e10 (last full one at 9.5e10: 25.1 s) before trusting R5/R7/R8 (all scaled from 4e10 data).

**Caveats.** ecalc NTT/batch numbers are from earlier code and aac6/aac7; a37v1's from the spec's node; treat "1.45x faster" as indicative. The #77 division model used unit costs inferred from S19B (low confidence). Nothing was run. Decision for the user: only R1 (2-4 d for -7...-12 s at the target (mod), R2 as half-day gate).

## AP34 — the apumult A1-A34 optimization list against ecalc (2026-10-23)

Read-only review of a later apumult list (34 items, "optimizations not present in ntt") against `main` 4b5bb14 and the
verdicts above. No code run; numbers are quoted with their labels. Verdicts: **new** (a TASKS "Proposed" row),
**have** (ecalc already does it), **closed** (measured or rejected before), **n/a** (does not apply to ecalc's design).

| item | apumult technique | verdict | why |
|---|---|---|---|
| A1 | GPU seed kernel (512 products per leaf) | new (re-gate) | A37-R1 closed NO-GO because mapping bound init under the node-process layout (S22, +0.55 s); one process per GPU maps about 7x faster, so the seed may bind now: TASKS row |
| A2 | Stirling-balanced leaf boundaries | closed | rejected in A37CMP (#20); `BS_SEED_FILL` already sizes spans from `lgammal` per run |
| A3, A7, A10, A32 | M-2 extent recurrence, d-scaled margin, per-level extent probes, single-pass helper | n/a | regions are laid out by an exact model (`mem_model.py --check-c`, within 0.6 GB measured) on VMM arenas that remap; the open margin lever is TASKS `BS_ARENA_ROOM` |
| A4 | ping-pong dual arena | have | two level pools of alternating parity (ALGORITHM §3, §8) |
| A5 | Newton truncation 2j+3 | n/a | ecalc's correction form takes 2j+2 limbs with the guarded anchored chain (error ≤ 8 units, results/DIVISION.md §RECIP); wider is not faster |
| A6 | Barrett tail high product with skipped sub-products | have | high products skip pieces below the cut (`rns_mul_high_db`, `grid_piece_skipped`), the reciprocal's products are band/high cuts |
| A8 | budget-gated background checkpoint | n/a | the only checkpoint (`ECALC_CKPT_TOP`) is off by default; the output writer is already hidden under the division |
| A9, A18, A19, A20, A33 | adversarial kernel harness, gate ladder with fault grep, soak, T2 windows, gate-removal audit | n/a (method) | not performance items; ecalc has TESTING.md gates, T1 residues on 8 primes plus T2 windows, the switch registry audit; the soak gap is TASKS "G2 soak" |
| A11 | P and Q arenas split over dies | closed | rejected (#24: local HBM 3.8 TB/s against 93 GB/s remote, m); ecalc already spreads numbers over the four APUs (`dbig` quarters, regions) |
| A12, A13 | zero-copy root P, Q and A as arena views | have | P, Q pass to the block pool without copy; A = S·B^dl is never formed |
| A14 | asynchronous pinned-staging pre-grow | closed | init reorderings K8 each lost 2.0-11.5 s (m); remaining start-up items are TASKS "Default start-up time" and "Start-up shortening" |
| A15 | three-tier dispatch thresholds | new (probe) | school tier measured and removed (loss 42-59 s, m); the batch/device boundary (`BS_MDEV_LOGL`) was never timed: TASKS row |
| A16 | shared operand's forward transform reused by the second product | new | the batch tier pairs products that share an operand, but the device top levels and the tree levels transform Q_B twice (the transform cache empties after each product): TASKS row |
| A17 | two-term reciprocal modmul, one correction | have | `mm_lazy` (ALGORITHM §4.2); apumult's g3r3 is the slower form |
| A21-A24 | per-node leaf ranges, log2 P reduction tree, β-transpose and c_top·log2 P models | have | node ranges and `MN_GROUPS` tree (ALGORITHM §3, §6.5); `estimate.py` / `mn_model.py`; ecalc's measured multi-node cost is the exchange, not c_top |
| A25, A26, A27 | fused CRT + carry, added operand fused into the CRT, CRT writes the result in place | have | `k_crt_batch` / `k_crt24` with LDS carry-lookahead, P₂ as the added operand, results written into the destination `dbig` |
| A28, A30, A31 | `HSA_XNACK=1` hipMalloc arena; SDMA and GPU-write hazards under XNACK | closed / n/a | XNACK=1 was 2x slower (1e10 80 -> 159 s, m); with XNACK off the two hazards do not arise (GPU writes go to device memory or registered pinned pools) |
| A29 | 101 GB usable per die | have | measured device edge 93.36 GB per APU, `ECALC_DEVICE_GB` budget and the memory fit (ALGORITHM §8) |
| A34 | ω derived from an order-2^44 root | have | `ec_root` derives any 2^logn up to v2(p - 1) = 44 from the generator; `tests/t_roots` checks order and the root chain |

Side finding: ALGORITHM §4.5 still said the transform cache served the reciprocal's last doubling and the division's X·Q
(stale since the held slots were removed 2026-10-11); corrected in the same merge.

**Measured follow-ups (2026-10-23, aac7 item 9zu4, main 53778c7, 1 node, 1e11 digits, every run VERIFY OK).**
- A1, start-up timeline (`ECALC_INIT_TL=1`), layout D, 2 runs: plane pools done 7.5 / 6.2 s, last background mapping 37.4 / 37.9 s,
  seed thread ends 51.5 / 52.0 s (seed − mapping +14.1 s both), bs level 1 starts 52.0 / 52.4 s; totals 142.3 / 139.2 s (init 38.5 / 38.9,
  bs 61.6 / 56.5, dm 42.2 / 43.7). Layout P (`ECALC_PE_PER_GPU=1 RNS_BATCH_SPLIT=1`): seed threads end 21.7 / 19.9 s, mappings 24.0 / 13.5 s,
  but both runs stopped at the host budget check (rc 8) before bs, so P has no full run. Without `RNS_BATCH_SPLIT=1` the planner refuses
  P at 1 n at both 1e11 and 5e10 (item 9zu3).
- A15, `BS_MDEV_LOGL` 28 / 29 / 30 / 30 (layout D, one run each): totals 135.07 / 127.45 / 136.89 / 131.62 s; bs 61.3 / 57.0 / 57.1 / 56.5 s.
  The batch tier is faster than the device tier at the levels that move (level 22: batch 1.80 s vs device 3.87 s; level 23: 2.86 vs 3.83 s),
  so 28 loses about 4 s of bs and 29 equals 30 within noise; the default stays (OPPORTUNITIES, rejected).
- A1 under P (Q93 rerun, aac7 item 9zu5, main 1e2ea1b, 1 node, 7e10 digits, P = `ECALC_PE_PER_GPU=1 RNS_BATCH_SPLIT=1`; 3 P runs and
  2 D runs, alternating, the third D run cut by the item's time cap; every run VERIFY OK, same sha). Seed thread ends: D 33.7 / 30.9 s,
  P 19.3 / 21.7 / 19.3 s; last background mapping: D 25.5 / 21.6 s, P 28.8 / 13.9 / 17.0 s; seed − mapping: D +8.2 / +9.3 s,
  P −9.5 / +7.8 / +2.2 s; bs level 1 starts: D 34.0 / 31.2 s, P 29.2 / 22.2 / 19.8 s. So P's per-APU seed threads start bs about
  10 s earlier, and in P the mapping, not the seed, binds in one run of three. Totals: D 207.1 / 198.0 s, P 444.5 / 373.2 / 369.9 s
  (bs 38.7 / 37.8 vs 98.0 / 58.0 / 58.4; dm 27.8 / 20.0 vs 107.1 / 63.7 / 63.4; recip 5.4 / 4.4 vs 25.8 / 16.1 / 16.0), so at one node P is
  about 1.8x slower overall and the start-up gain does not matter there; P is the target-shaped layout, not the 1-node one.
- BS_SEED_NUMA (Q92: the layout-D seed by four per-APU workers pinned to their NUMA node, as P seeds; aac7 items 9zu6, 9zuf-9zuj,
  1 n): 1e9 digits identical; at 1e11, alternating 1/0/0/1, seed ends 56.7 / 41.7 / 49.2 / 40.1 s, bs level 1 starts 57.1 / 42.4 / 49.9 /
  40.4 s, totals 145.4 / 133.4 / 137.9 / 130.8 s (init varies 26-43 s between runs). In every run the seed ends 13.8-14.4 s after the
  region pools exist (the pull kernels' launch): the seed waits for the pools, not for CPU speed or NUMA placement. Dropped (Q94 (b)).


Sections moved to `archive:results_merged_2026-10-24/APUMULT.md` (2026-10-24 cleanup): apumult_summary; apumult_F1; apumult_F2; apumult_M; apumult_R.