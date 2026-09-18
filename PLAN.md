# PLAN.md — the single plan for `~/ntt`

One file for everything: the MI300A microbenchmark suite, the reproduction of
the *e*-to-40-billion paper, and the path from there to `~/ntt`'s own design.
Supersedes the earlier `PLAN.md` (suite only) and `EPAPER_PLAN.md`. Edit in
place; append to the status log (§14) as items land.

Written 2026-09-12; revised 2026-09-12 (evening) after Phases 0, 1 and 1b —
all thirteen discrepancies decided; the `s25-40` follow-up is archived. Sources: `bench/01`–`23`, `common_ntt.h`, `RESULTS.md`
§1–30, `DESIGN.md`, `ALGORITHM.md`, and `~/apucode/epaper.pdf` ("High-Performance
Computation of e to 40 Billion Decimal Digits on a Single MI300A Node", 4 pp.,
scanned). A mirror of this file lives at `~/apucode/PLAN.md` on the laptop.

Contents
```
 1. Environment
 2. Inventory of existing code
 3. Assessment of the existing benchmark set
 4. The paper: what it specifies, how it differs from our design, open questions
 5. Phase 0 — environment checks                     (minutes)
 6. Phase 1 — hardware gate benchmarks               (~1 week)
 6.3 Phase 1b — discrepancy investigation           (~2 sessions)
 7. Phase 2 — suite infrastructure and known gaps    (~1 week, overlaps 1)
 8. Phase 3 — implementation of the paper's pipeline (~4 weeks)
 9. Phase 4 — verification and acceptance
10. Phase 5 — experiments: the paper's structure vs ours
11. Phase 6 — broader MI300A capability benchmarks
12. Conventions for every benchmark and test
13. Open decisions
14. Status log
```

---

## 1. Environment

| | |
|---|---|
| login | `aac6.amd.com` → `aac6-fe1` (no GPU, ~80 users; never compute here) |
| account | `chcoppola` / `mpo_2026`, home `/shared/prerelease/home/mpo_2026/chcoppola` (NFS, **not backed up**) |
| partition | `PPAC_MI300A_SPX`: `ppac-pl1-s24-[16,26,30]` idle, `s24-35` down. 4 × MI300A (gfx942, 228 CU, 128 GiB HBM each), **SPX / NPS1**, 4 NUMA nodes × 128 GB (distance 10/32), 192 logical CPUs (96 Zen4 cores), 502 GB, 550 W per APU, 8 h walltime cap. All three nodes identical (RESULTS.md §17, §29); campaign 4 uses `s24-16`. **Always pass `-p`** — the default partition is down. |
| other nodes | `SH5_MI300A_SPX` `sh5-pl1-s12-33` (1 APU, 128 GiB); `SH5_MI300A_CPX` `sh5-pl1-s12-[09,12,15,36]` (1 APU as 6 devices × 21.3 GiB); `PPAC_MI300A_CPX` `ppac-pl1-s25-40` (4 APUs as 24 devices, **down**, see D6). No node is in NPS4. |
| limits found | `ulimit -l` 31.4 GiB (not enforced for `hipHostRegister`), `ulimit -m` 450 GiB, `amdttm.pages_limit` 512 GiB, THP `always` |
| toolchain | `module load rocm` → ROCm 7.2.4 (6.3.x, 6.4.x, 7.0–7.14, 10.0 also available). `hipcc`, clang 22, OpenMP. GMP 6.x installed (`-lgmp`). No MPI without the module. |
| build | `make` in `~/ntt` (`hipcc -x hip -O3 --offload-arch=gfx942 -fopenmp`) |
| run | `./run <binary> [args]` = `srun -p PPAC_MI300A_SPX -N1 --gpus=4 --time=00:20:00`; env `P=`, `G=`, `T=` override. Long runs: `salloc … --time=8:00:00` inside `tmux`. |
| target | `xnack-` default; no managed-memory paging unless built `xnack+` |
| reference | `~/apucode/aac6-environment.md` for the full survey |

---

## 2. Inventory of existing code (`~/ntt`)

### Documents
| file | lines | role |
|---|---:|---|
| `DESIGN.md` | 625 | original design for *e* on one node (2 × 62-bit Shoup, four-step, push corner turn); §18 lists measured revisions |
| `RESULTS.md` | 901 | campaigns 1–3 (§1–16) and campaign 4 (§17–30: Phase 0, Phase 1 gates, harness, burst clock, baseline, partition survey, Phase 1b verdicts), with CORRECTIONs to DESIGN.md and to campaigns 2–3 |
| `ALGORITHM.md` | 654 | master guide: segments S1–S16 with option tables, tiers 1–3, reviews R1–R11 |
| `PLAN.md` | 676 | this file |
| `bench/README.md` | — | program → question → RESULTS.md section |
| `results/` | — | `0_env_*.txt`, `0_params.txt`, `0_node_*.txt` (Phase 0 / partition survey); `<date>_<node>/` suite runs with `*.log`, `*.smi` telemetry, `env.log`, `results.tsv`; `1b_*` Phase 1b runs |
| `tests/t_params.c` | 112 | GMP check of every constant in the paper and in `bench/` |

### Harness
- `bench/common_ntt.h` — `HIP_CHECK`, `timer_events()`, `device_count()`,
  `header()`, `report_sum()` / `report_max()` (per-APU + node), and since
  campaign 4 `meta(name)` (one provenance line: host, HIP, ROCm path, max
  sclk, devices, date) and `result()` (`RESULT <bench> <metric> <unit> <node>
  <apu…>`, emitted by every `report_*`). Plain C, no STL.
- `bench/ntt_kernels.h` — the 62-bit primes, `smul`, `FB/FWD3/GB/GINV3/EXW/EXR`,
  `mulmod/powmod/shoup_pre/mont_j/brv/build_table`, `hash61`. Used by
  06, 07, 12, 13, 14, 22.
- `./suite [-w node] [-t time] [-p partition] [bench…]` — builds, takes one
  `salloc` on the node, runs each program as an srun step with an `amd-smi`
  JSON sampler (2 s) alongside, writes `results/<date>_<node>/`. Default set
  is the current programs (not 04/05/08). Per-program arguments in
  `bench/<name>.args`. `./diff A B` prints per-metric ratios.
- `envcheck.sh`, `nodecheck.sh` — Phase 0 environment and partition surveys
  for a compute node.
- `make isa B=<bench> K=<kernel-substring>` + `isa.py` — instruction-class
  counts per kernel and per innermost loop from `--save-temps` output
  (`isa/`).
- `Makefile` — `bench/%` from `bench/%.c` with `hipcc -x hip`; `20_cpu` and
  `tests/%` link `-lgmp`.

### Programs — `bench/NN_name.c`, one question each
| # | file | lines | question | key result | status |
|---|---|---:|---|---|---|
| 01 | `01_butterfly.c` | 269 | which arithmetic engine (Shoup u64 / Montgomery / Goldilocks / Shoup FP64)? | Shoup u64 240 949 Gbit-bfly/s node; FP64 most bfly/s but fewer bits | current |
| 02 | `02_capacity.c` | 163 | how much memory can the NTT own, at what BW, which allocator? | 460 GiB via `hipHostMalloc`, 14.0 TB/s read, 11.8 triad; 63 s alloc + 40 s touch | current |
| 03 | `03_fabric.c` | 198 | all-to-all corner turn: allocator, push vs pull | push 697 vs pull 399 GB/s; allocator irrelevant | current |
| 04 | `old/04_ntt_lds.c` | 323 | simple LDS radix-2 NTT, verified vs schoolbook | 1 554 Gbfly/s (40 % of bound) | superseded by 06, `bench/old/` |
| 05 | `old/05_ntt_reg.c` | 304 | register-blocked (8 pts/thread, 3 exchanges) | 2 211 Gbfly/s | superseded by 06, `bench/old/` |
| 06 | `06_ntt_opt.c` | 412 | + XOR swizzle, LDS-staged twiddles, twiddle ablation | **2 325 Gbfly/s** fwd; swizzle +2 ± 3 % (§30); twiddles cost 21 % | current |
| 07 | `07_ntt_tw.c` | 243 | twiddle traffic: interleaved ulong2 / non-temporal / LDS-staged | ulong2 ±0 (§30, was +6 %); NT 0.66×; LDS 0.96× | current |
| 08 | `old/08_multiply.c` | 530 | complete verified 2-prime multiply, simple kernels | 1.98 ms / 60.6 M digits; 1.66 B/digit | superseded by 13, `bench/old/` |
| 09 | `09_logic.c` | ~230 | per-op VALU throughput and true latency, cycles at the in-kernel clock | `v_mad_u64_u32` 0.99 cyc; Shoup 11.4; bfly 20.3; add64 latency 26 (§37) | current |
| 10 | `10_lds.c` | 230 | LDS BW by width, bank-conflict classes, shuffle, barrier | 45 TB/s `b128`; stride 16 = 0.10×; odd strides free | current |
| 11 | `11_fabric_ntt.c` | 260 | fabric width / chunk / stride / overlap | **909 GB/s at 64-bit stores**; flat from 128 B; 74 % overlap | current |
| 12 | `12_ntt_inv.c` | 269 | register-blocked Gentleman-Sande inverse | **2 569 Gbfly/s** | current |
| 13 | `13_multiply2.c` | 411 | fused multiply, fast kernels both directions | **1.08 ms / 60.6 M digits** on s24-16 (1.23 in campaign 3); 79 % inside row kernels | current — the production multiply |
| 14 | `14_sustained.c` | 255 | trivial-twiddle specialisation; sustained run, optional resident footprint | 3.3 % slower with fewer instr; 1.0 % decay/300 s at 256 GiB; 550 W cap, ~1.49 GHz | current |
| 15 | `15_barrett_f64.c` | 325 | the paper's FP64-Barrett modmul: exactness for four operand ranges; rates with in-kernel clock; D1 variants (rint, 3 corrections, 1/2/4 chains, 1/3 blocks per CU) | exact with 2 corrections only for ≤ one lazy operand in [0,2p); **1 330 Gmodmul/s/APU at 1.59 GHz** (paper 775), 22 VALU/modmul | current (campaign 4) |
| 16 | `16_ntt_tile.c` | 566 | the paper's tiled DIF NTT: runtime kernel (+ twiddle modes for D4) and template kernels `k_b16t<STG>` / `k_b1t<LGL>` with 2048-element blocks (D5); verified vs host DIF for 14 configurations incl. batched 2¹¹/2¹⁴ | **2³¹ in 117 ms = 1.17 TB/s** (paper 1.08); batched 1 450 / 1 380 / 1 520 GB/s at log L = 14 / 17 / 20 | current |
| 17 | `17_hostreg.c` | 195 | `hipHostRegister` staging by NUMA placement; single-stream D2H | local = HBM speed (3.7 TB/s); remote 55× slower; 64 GiB registers in 4.8 s | current |
| 18 | `18_staging.c` | 128 | kernels reading (expand) / writing pinned host staging | expand 0.6–0.8 TB/s; kernel store 1.8 TB/s | current |
| 19 | `19_peer_gather.c` | 114 | 4-way interleaved peer read (GPU CRT pattern) | 154 GB/s per APU; APU2 blit copy-in at half rate | current |
| 20 | `20_cpu.c` | 343 | CPU Garner CRT + striped carry (GMP-verified) in five variants (D8), repack, STREAM, seed spans, GPU interference | **2³⁰ CRT 0.29 s** with the tight carry window (was 0.63); FP64 Garner 1.8× slower; no interference | current |
| 21 | `21_alloc.c` | 120 | allocation / registration / first-touch cost vs size; calloc on reuse (D10) | `hipMalloc` 37 ms/GiB; calloc free | current |
| 22 | `22_clock.c` | 125 | effective shader clock during VALU bursts from `clock64()/wall_clock64()` | 1.47–1.60 GHz whole-APU on every node and mode; 1 956 MHz for a lone CPX partition | current |

| 23 | `23_d2h.c` | 80 | D2H blit vs streams × in-flight × chunk × target (D7) | 262 GB/s per APU with 8 streams (58 single-stream) | current |
| — | `mem/infcache.c` | ~110 | scattered gather + pointer chase vs working set | IC 300 ns / HBM 650 ns; gather 272 → 226 GB/s across 256 MB (§37) | current |
| — | `arith/mfma.c` | ~100 | int8 / bf16 / f64 MFMA rates with in-kernel clock | 700 / 390 / 21.6 TMAC/s per APU, power-limited (§37) | current |

Shared primitives now live in `ntt_kernels.h` (done in Phase 2.1); 04, 05
and 08 keep their own copies and are superseded.

### Reusable building blocks for the paper's pipeline
- register-blocked fwd/inv row kernels (06, 12, 13) — alternative b16 body
- Montgomery pointwise, two-level twiddle, in-place transpose (13)
- hash61 homomorphism verifier (08, 13)
- 4-APU concurrent measurement pattern (all programs)
- push corner turn at 64-bit (11) — alternative to one-prime-per-device

---

## 3. Assessment of the existing benchmark set

It is a **design-driven characterisation campaign** for one workload, not a
general suite. Every program answers a stated question and records its verdict.

### Strengths — keep
- Hypothesis → measurement → verdict, with explicit CORRECTIONs.
- Correctness checked: schoolbook convolution, round trip, hash61 at full size.
- Sound method: 8 independent chains vs 1 for throughput vs latency; best-of-N
  behind an OpenMP barrier so all four APUs run concurrently; `report_sum`
  vs `report_max`; `cyc/op` in lane-cycles so figures compare to the ISA.
- Small, clean harness.

### Weaknesses — status after campaign 4
1. ~~**No output capture.**~~ **Fixed**: `META`/`RESULT` lines, `./suite`,
   `results/<date>_<node>/` with telemetry (§7.1).
2. **Chronological numbering, not structure.** Primitives extracted
   (`ntt_kernels.h`); 04/05/08 still build but are out of the default set;
   `bench/old/` and the category layout remain to do (§7.1).
3. **One shape only.** N = 2048, 256 threads, 8 pts/thread, p < 2⁶², L = 2048².
   Nothing sweeps occupancy, block size or points-per-thread.
4. **Reproducibility.** `./suite` handles modules and node pinning; `make
   isa` now reproduces instruction counts in-tree (§31). The campaign-1 ISA
   claims (28 instr, 272 `v_mov`, 147 `s_nop`) are still un-rerun and assumed
   the wrong clock.
5. **Acknowledged, unresolved.** 09's add/sub latencies strength-reduced;
   latencies per wave-slot; the int8 MFMA number in ALGORITHM.md R10 has no
   source in `bench/`. 14 now runs 300 s at 256 GiB with telemetry (§23).
6. **New (§24, §29): every cyc/op in campaigns 2–3 assumed 2.10 GHz; the
   shader runs at ~1.55 GHz under this load.** Restated ×0.74; report rates
   from now on.

---

## 4. The paper

### 4.1 What it specifies
| item | paper's choice |
|---|---|
| series | e = Σ 1/k!, N = min{m : lgamma(m+1)/ln10 ≥ d+50} ≈ 3.06×10⁹ at d = 4×10¹⁰ |
| recursion | 2-variable: P(a,b) = P₁Q₂ + P₂, Q(a,b) = Q₁Q₂; P, Q ≈ 2.08×10⁹ limbs (16.6 GB) each |
| phases | bs → 10dP → dm → T1 → dc → T2, sequential |
| bigint | little-endian base-2⁶⁴ limbs; repack width b = 64 (one limb per NTT point); `canon64` Barrett-reduces limbs to [0,p) with μ₁₁₅ = ⌊2¹¹⁵/p⌋ |
| RNS | 4 primes < 2⁵², p ≡ 1 mod 2³³: 3923057487904769 (g=3), 3641582511194113 (g=5), 2867526325239809 (g=3), 2586051348529153 (g=10); Σlog₂p ≈ 206 |
| modmul | FP64 Barrett + Dekker split, 16 FP64 ops, **two** corrections each direction (single correction fails 0.57 % for P[1]); ~775 Gmodmul/s/APU "at 12.4 TF64" |
| NTT | DIF forward natural→bitrev, DIT inverse bitrev→natural, pointwise on bit-reversed data; stages tiled into passes of STG = 7 (`NTT_B16_STG`); `ntt_tile_dif_b16<STG>`: block 256, TILE = 128, LDS `sh[TILE*17+1]` = 17 416 B, 3 blocks/CU; bb = tid&15, tt = tid>>4; per-thread twiddles computed; 4 passes at log n = 31 (3 × b16 + 1 × b1 for stages [0..9]); ~1.08 TB/s effective |
| fusions | scale-fuse (×n⁻¹ + canon in last inverse store); pointwise-fuse (`PW_FUSE`, log L ≥ 14) |
| multiply tiers | **mdev** (L ≥ 2¹⁸): one product, **one prime per device**, 64 GB pinned `hstage_buf[4]` (malloc + `hipHostRegister`, OpenMP first-touch), per device copy+canon → fwd → pointwise → inv → D2H, CPU CRT+carry; **mdev_pair** (shared B transformed once); **batch** (L_sub < 2¹⁸; tile M = min(N, ⌊15×10⁹/(3·L_sub·8)⌋); `scatter_expand_k` reads pinned host directly; GPU S-stripe CRT when M_t ≥ 8); **grpB** (≤ 8 unique B pointers) |
| CRT | CPU `crt_carry_par4`: Garner with M₁ = P₀P₁ (128-bit), M₂ = M₁P₂ (192-bit), x86 `divq`; T = min(96, nab/1024) stripes; 32-limb spill; sequential spill merge. GPU `crt4_gpu`: S = max(1, ⌈GPUCRT_MIN_BLOCKS/M_t⌉) stripes, block 64, peer-reads all four `dev_da[c]`, 8-word spill |
| Newton | `newton_recip_seeded`: r₂ = r², take = min(2k+2, n), Q_t = Q >> (n−take), Qr₂ = Q_t × top(r₂), T₁ = Qr₂ >> (take − 64·skip), T₂ = r << (k+1), r = T₂ − T₁, k = 2k; overshoot (T₂ < T₁) → shrink r by 1/16, retry; `NEWTON_R2TRUNC` final iteration only. `newton_divmod`: K = bits(A) − bits(Q) + 2, X = mul_hi_shift(A, μ, n+k), down-correct while XQ > A, up-correct while R ≥ Q |
| bs orchestration | bottom-up BFS; seed spans of 512 terms via OpenMP schoolbook; per level: max_nl ≤ 160 → schoolbook; L_sub < 2²⁸ → batch_pair; npairs ≤ 128 ∧ 2·max_nl ≤ MSL (2.1 G) → mdev-serial; else mdev-parallel; level pools freed per level |
| dc orchestration | top-down BFS Barrett division by 10^h; divisor cache (h, T = 10^h, μ ≈ 2^(n+k)/T, n, k); prewarm bottom-up: T(h) = T(h/2)², μ-square seed at k₀ = 2(n_j+k_j)−n shifted to K/2+8, K = ⌈3.33h⌉+64, one seeded Newton doubling; split pieces with dig > 19 as (⌊val/T⌋, val mod T); tiers: nsplit ≤ 2 → TOP; max_nl < 256 → LEAF (GPU 128-bit Barrett by 10¹⁸, μ = 10633823966279326983, ≤ 3 conditional subtracts, 18-digit blocks high→low); max_L_sub < 2³⁰ → DEEP; else MID |
| verification | T1: 8 fixed 62-bit primes qᵢ; P, Q mod qᵢ by the recursion in ℤ/q (chunked OpenMP, serial combine); X, R mod qᵢ by Horner; check 10^d·P ≡ X·Q + R (caught a ROCm grid-overflow bug). T2: 50-char windows at {50, 10⁶, 10⁸, 10⁹, 10¹⁰, 4×10¹⁰} vs known digits |
| memory | peak RSS ≈ 256 GB: hstage 64 GB pinned (pregrow 2³¹); ch_da/ch_db 128 GB device (pregrow 2³¹); dev_da/dev_db 64 GB (pow2-grow to 2³⁰); P,Q,A,X,R ≤ 50 GB; g_AXpool ≤ 33 GB; level pools ≤ 35 GB. Grow-only never shrink (hipFree+hipMalloc 0.5–1 s each); pow2 grows so bs's 6.25×10⁸ request lands at 2³⁰; malloc not calloc (−5 s); never hold > 35 GB excess host memory |
| results | A22: 285.7 s = bs 112.2 + 10dP 12.6 + dm 46.8 + T1 ≈ 3 + dc 110.3. Ladder v4 43 546 s → A17 1 290 → A18 594 → A19 431 → A20 349 → A21 287.5 → A22 285.7. Batched NTT BW 103 GB/s @ log L = 11, 1 146 @ 14, ~1 300 @ ≥ 17. Estimated floor 270–275 s |
| dead ends | NUMA interleave on hstage (+20 s); persistent Rhi depth-pool (+11 s); Karp-Markstein truncation (error saturates at 2⁻ᵍ); u64 pseudo-Mersenne primes (3.1× slower NTT); 8 × u32 Montgomery (4.83× slower CRT) |
| future | cyclic/negacyclic NTT for true Karp-Markstein (−10 s); 2D split for dc TOP; radix-4 stages; multi-node via 4-step NTT |

### 4.2 Differences from `~/ntt`'s design (ALGORITHM.md)
| | paper | `~/ntt` (measured) |
|---|---|---|
| primes | 4 × 52-bit, FP64 Barrett | 2 × 62-bit, integer Shoup (bench/01: 240 949 vs FP64 210 602 Gbit-bfly/s) |
| density | 64 bits per 4 × 8 B = **2.0 bits/byte** | 45 bits per 16 B = **2.81 bits/byte** |
| transform | one long transform (2³¹), tiled 7-stage passes, 4 read+write passes/direction | four-step, N = 2048 register-blocked rows, 3 plane-touches/direction |
| multi-APU | one prime per device, **no fabric traffic** | four-step corner turn over xGMI (909 GB/s, 74 % overlap) for level 0 |
| CRT | CPU Garner, 96 threads | host Garner, flagged as the cliff (S11) |
| radix conversion | top-down Barrett with cached divisors + reciprocals | scaled remainder tree, no reciprocals |
| bs scheduling | bottom-up BFS with level pools | level-synchronous batching (S13a) — same idea |
| memory target | 256 GB peak, 4×10¹⁰ digits | 460 GiB claimable, 10¹¹ digits |

"One prime per device" is the most consequential difference: it removes the
corner turn at the cost of 4 × 16 GiB planes per array. Treat it as a serious
alternative to S9/S16, not just something to reproduce.

### 4.3 Inconsistencies to resolve before trusting the paper's numbers
- **Q1. Transform-length ceiling vs 10dP.** With b = 64, 10^d·P (2.08×10⁹ ×
  2.08×10⁹ limbs) needs n ≥ 4.16×10⁹ = 2³² points, but `ch_da/ch_db` are
  pregrown to 2³¹ (which is what gives the stated 128 GB). Options: (a) pools
  really are 2³² (256 GB device — the whole stated peak, unlikely); (b) A as
  two half-products; (c) fold 10^d into the reciprocal so A is never
  materialised: X = ⌊P · (10^d·μ)⌋. Settle in Phase 3 step 3.
- **Q1 status: decided.** Default (i): pools at 2³¹ and a Karatsuba top-level split (`rns_multiply_split`) for any product over 2³¹ points — the only reading consistent with the paper's 128 GB and 12.6 s (§35). Alternative (ii), pools at 2³² with no split, stays as a switch for a possible follow-up paper.
- **Q2. Convolution bound.** 2·64 + 32 = 160 ≤ 206 bits: 46 bits of slack.
  Three primes (≈156) do not fit at 2³², so four is forced by b = 64, not by
  range. b = 48 with 3 primes saves 25 % of plane memory — Phase 5 item.
- **Q3. "1.08 TB/s effective"** is far below bench/02's 14 TB/s and bench/06's
  ~6.3 TB/s. The paper's NTT is likely compute-bound on the FP64 modmul;
  bench/01 predicts ~1 050 Gbfly/s per APU for FP64 vs their 775 Gmodmul/s.
  "12.4 TF64" = 775 G × 16 ops, i.e. ~28 % of the measured `fma64` rate
  (bench/09: 22 183 Gop/s = 44 TF). **Resolved by B2 (§25):** LDS/modmul-
  bound at 1.0 TB/s, 27 % of HBM; the modmul runs 1.72× the paper's rate in
  isolation (§19), and neither the chain nor the in-kernel rate reproduces
  "775" (D1 open, needs the ISA count).
- **Q4. Two corrections per direction** depends on the primes and the
  rounding of hi·pinv. **Resolved by B1 and D2 (§19, §30):** two corrections
  are exact only for ≤ one lazy operand in [0, 2p); both lazy, or one in
  [0, 4p), is wrong 0.4–21 % of the time. The 0.57 % figure itself is not
  reproducible with uniform inputs.

---

## 5. Phase 0 — environment checks (minutes; on a compute node, not the login node)

- [x] `numactl -H`, `lscpu`, `free -g`, `ulimit -l` under `srun`: NUMA layout
      (expect 4 nodes, one per APU), 96 physical cores, ≥ 500 GB, **unlimited
      locked memory** (needed for `hipHostRegister` of 64 GB).
- [x] `module load rocm`; record ROCm 7.2.4, HIP 7.2.53211, clang 22. Keep
      6.4.3 available for A/B if gate B2 misses.
- [x] Confirm `xnack-` is fine: the paper uses `hipHostRegister` + `hipMalloc`,
      no managed memory.
- [x] GMP sanity (`tests/t_params`): the four primes are prime and ≡ 1 mod
      2³³; g has the stated order; μ₁₁₅ for each prime; μ = ⌊2¹²³/10¹⁸⌋ =
      10633823966279326983; ω₃₃ = g^((p−1)/2³³) has exact order 2³³.
- [x] `sinfo` node states; pick one node (`-w`, chosen: `ppac-pl1-s24-16`) and use it for every number
      in Phase 1 so results are comparable.
- [x] Walltime: 8 h is ample for the 286 s run and for the 10⁹-digit GMP
      reference (Phase 3 step 0).

---

## 6. Phase 1 — hardware gate benchmarks

### 6.1 Already answered by `bench/01–14` — reuse
| paper claim | existing measurement | verdict |
|---|---|---|
| FP64 modmul viable, ~775 Gmodmul/s/APU | bench/01 `shoup_f64` 4 212 Gbfly/s node ≈ 1 050/APU; bench/09 `fma64` 1.38 cyc, `rint64` 3.02 cyc | plausible; paper is ~75 % of a Shoup-FP64 butterfly |
| LDS pad 17 conflict-free for 64-bit | bench/10: strides 17, 33 → 0.98–0.99× | confirmed |
| 3 blocks/CU at 17 416 B LDS | 64 KiB LDS/CU (bench/06/07) | 3 × 17 416 = 52 KB, fits |
| ≥ 500 GB usable at HBM speed | bench/02: 460 GiB at 14.0 TB/s | confirmed |
| peer access, allocator-independent | bench/03: within 1 % | confirmed |
| no throttling | bench/14: 0.5 % over 60 s | partial — paper runs 286 s (B7) |
| launch cost for batch/LEAF tiers | 4.0 µs dispatch (ALGORITHM.md ✔) | known |

### 6.2 New gate benchmarks, priority order
Numbering continues the `bench/` sequence. Each prints `VERIFY`, `RESULT`,
`META` lines (§12).

- [x] **B1 `bench/15_barrett_f64.c` — the paper's exact modmul.** The 8-line
      FP64-Barrett/Dekker with two corrections, on the paper's four primes;
      Shoup-u64 on the same primes for comparison. Checks 775 Gmodmul/s/APU;
      exactness vs `__uint128_t` over 10⁹ random pairs + edge values
      {0, 1, 2, p−2, p−1, 2ᵏ, p−2ᵏ}; single-correction failure rate.
      **Pass:** ≥ 700 Gmodmul/s; zero mismatches with two corrections;
      one-correction failures ≈ 0.5 % for P[1].
- [x] **B2 `bench/16_ntt_tile.c` — tiled DIF b16 kernel prototype.**
      TILE = 128, pad 17, bb/tt mapping, per-thread twiddles, STG = 7, plus
      the b1 pass; one pass over 2²⁸–2³¹ points. Checks 1.08 TB/s effective,
      4 passes/direction at log n = 31, 3 blocks/CU, batched BW 103 → 1 146
      → 1 300 GB/s at log L = 11/14/≥17. **Pass:** within 15 % of each; bit-
      identical output for STG ∈ {5,6,7,8}. Decides Q3 (compute- vs memory-
      bound). Also measure per-thread twiddle exponentiation vs a small
      ω^(2^j) table.
- [x] **B3 `bench/17_hostreg.c` — `hipHostRegister` on 64 GB of malloc'd
      memory.** Registration success and time; device read/write BW into it;
      OpenMP first-touch interleave vs `numactl --interleave` vs node-local
      (paper: interleave policy +20 s). **Pass:** registers; node-local reads
      ≥ bench/02's host-memory rate; interleave measurably slower. **If
      registration fails at 64 GB the staging design must change — the
      biggest environment risk.**
- [x] **B4 `bench/18_staging.c` — D2H and staging traffic.** Four devices
      simultaneously `hipMemcpy` D2H 16 GiB each into pinned host; a
      `scatter_expand_k`-style kernel reading pinned host directly with
      zero-extension. **Pass:** aggregate D2H ≥ ~200 GB/s node; kernel-from-
      host ≥ 89 GB/s per device (bench/02 single-flow). *Result (§26, §30):
      expand 0.6–0.8 TB/s, kernel store 1.8 TB/s, blit 58 → 262 GB/s with
      8 streams.*
- [x] **B5 `bench/19_peer_gather.c` — fine-grained 4-way peer gather.** Each
      device reads element k from all four `dev_da[c]` (3 remote, 1 local),
      as `crt4_gpu` does. bench/11 showed scattered remote access collapses
      to 0.18×; this is contiguous per array but interleaved across sources.
      **Pass:** ≥ 100 GB/s per device; else the GPU CRT tier stages through
      a local copy first.
- [x] **B6 `bench/20_cpu.c` — CPU-side rates on the compute node.** 96-thread
      Garner CRT with 128/192-bit `divq` (limbs/s); 96-stripe carry with
      spill merge; OpenMP repack of 2³¹ limbs into hstage; STREAM; schoolbook
      seed spans of 512 terms; CPU–GPU interference while all four APUs run
      the NTT. **Pass:** CRT of 2³¹ limbs ≤ 3 s (else it dominates the 12.6 s
      10dP phase); interference ≤ 10 %.
- [x] **B7 extend `bench/14_sustained.c`** to 300 s with 256 GB resident and
      `amd-smi` clocks/power sampled. **Pass:** ≤ 2 % decay, no clock drop.
- [x] **B8 `bench/21_alloc.c` — allocation costs.** `hipMalloc`/`hipFree` vs
      size (paper: 0.5–1 s per multi-GB), pow2-grow, `malloc` vs `calloc`
      first-touch, `hipHostMalloc` vs `hipHostRegister` vs `hipMalloc` alloc
      + first-touch curves. Informs pool strategy; nothing fails on it.
- [x] **B9 component-sum model** of one 2³¹-point 4-prime mdev multiply from
      B1–B6, vs the paper's 10dP = 12.6 s. bench/13 scaled naively gives
      ~2 s for the GPU part, so ~10 s must be repack + H2D + D2H + CPU CRT.
      If the model cannot reach 12.6 s, resolve before building.

Critical path: **B3** (can we stage 64 GB their way), **B2** (does the NTT hit
their bandwidth), **B6/B9** (does the CPU CRT fit the phase budget).

## 6.3 Phase 1b — discrepancy investigation

Every difference between the paper and RESULTS.md §17–28 gets the same
treatment: state the three candidate explanations — **(D)** genuine
discrepancy with the paper, **(E)** our coding or measurement error,
**(A)** accounted for by a difference in configuration or interpretation —
name the test that separates them, and record the verdict here. A verdict
needs a measurement, not an argument. Order is by how much the answer would
change Phase 3.

### D1. Modmul rate: 1 330 vs 775 Gmodmul/s per APU (1.72×) — **verdict (D) — §31**: 22 VALU/modmul, 1.59 GHz, unreproducible by ILP, occupancy or rounding
- (E) Our chain loop is not the paper's op count: the compiler may have
  hoisted `pinv`, fused the two corrections into `v_cndmask`, or dropped a
  correction because the chain's values are provably in range.
- (A) The paper's 775 is measured *inside the NTT kernel* (with LDS and
  loads), not in a register chain; or it is per-butterfly, not per-modmul; or
  their "12.4 TF64" is a roofline they quote, not a measurement.
- (D) Their modmul has more work than the eight lines (e.g. `rint` instead of
  `floor` plus sign fix, or an extra Dekker step).
- **Tests:** (1) `make isa` on `k_rate_f64`: count VALU instructions per
  modmul; must be ≥ 16 FP64 ops. If fewer, the compiler simplified it — add
  `asm volatile` barriers and remeasure. (2) Measure modmuls/s *inside*
  `16_ntt_tile`'s b16 pass: 7 modmuls × n per pass / pass time. If that lands
  near 775, (A) — the paper's figure is the in-kernel rate. (3) Try `rint`
  and a three-correction variant; if the rate drops to ~775, (D) with a
  plausible cause.

### D2. Single-correction failure rate: 0.57 % (P[1]) vs 0.21 % (P1) / 0.37 % (P0) — **verdict (A) partly — §30**
- (A) Different operand distribution: the paper's inputs are lazy butterfly
  outputs whose distribution over [0, 2p) is not uniform; or their lazy
  operand is (u − v + 2p) unfolded, i.e. in [0, 4p).
- (E) Our failure count uses one seed per APU; a bias in the generator.
- (D) Their P[1] is not the prime we tested (transcription error in the paper).
- **Tests:** (1) Rerun `15_barrett_f64` with a ∈ [0, 4p) × canonical: if P1
  gives ≈ 0.57 %, (A) and the paper's kernel does *not* fold before the
  multiply — which then requires checking that two corrections still
  suffice at 4p (our data says they do not for both-lazy; for 4p × canonical
  it is untested). (2) Instrument `16_ntt_tile` to count one-correction
  failures on the real butterfly stream. (3) Verify with GMP that the four
  primes as printed are the only 52-bit primes ≡ 1 mod 2³³ with those
  generators near those values (they are prime, so (D) is unlikely).

### D3. FP64 lazy adds are inexact — the paper cannot be doing what it says — **verdict (A) — §30**
- (E) We misread: "lazy reduction in [0, 2p)" may describe the integer
  representation, with FP64 used only in the modmul — exactly what we ended
  up doing.
- (A) The paper's b16 kernel keeps `uint64_t` in LDS and converts at the
  multiply; the text simply omits it.
- (D) They add in FP64 and lose bits — impossible, tier-1 would fail.
- **Tests:** none needed beyond what §25 already shows; record as (A) with
  the omission noted. Add a unit test in Phase 3 that fails if any butterfly
  path adds doubles.

### D4. Per-thread twiddle exponentiation halves the speed — **verdict (A) — §32**: literal per-thread costs +13 %, per-block +9 %, table best
- (A) "Computed per-thread" may mean one modmul from two small tables (what
  we ended up with) — computed, as opposed to loaded from a full table.
- (E) Our exponentiation was 31 iterations for every thread; the paper's
  exponent may be structured so most bits are zero, or they compute one base
  per block and derive per-thread values by ≤ 4 multiplies.
- **Tests:** (1) Implement the block-base variant: one `dpow` per block
  (exponent for c = slab·16), then per-thread T = base × tab16[bb] — two
  modmuls. If it reaches the table version's speed, (A/E): the paper's
  description is consistent with a cheap scheme we didn't try. (2) Count
  modmuls per pass in each variant with `make isa`.

### D5. 2³¹ transform 1.0 vs 1.08 TB/s; batched log L = 14: 920 vs 1 146 — **verdict (E) — §33**: template STG + 2048-element blocks → 1.17 TB/s, 1 450 / 1 380 GB/s at log L = 14 / 17
- (E) Our pass planner puts a 4-stage, 16-point tile on log L = 14 (stages
  [10..13]); the paper's b1 kernel may take up to 14 stages with a larger
  LDS block, or its b16 template handles small STG efficiently.
- (A) The paper's "effective bandwidth" may count 2 × 8 B × n per pass
  *per direction including the pointwise pass*, or use 10⁹ vs 2³⁰ bytes.
- (D) Real: the paper has ~8 % more headroom from unrolled STG templates.
- **Tests:** (1) Make STG a compile-time template (7, 6, 5, 4) and unroll;
  remeasure. (2) Extend b1 to 2^12 and 2^14 points (32/128 KiB LDS — the
  latter needs 512 threads and 1 block/CU; measure both). (3) Recompute our
  figure under both byte conventions. Pass condition: within 5 % on all
  three of 2³¹, log L = 14, log L = 17, or a documented (D).

### D6. Shader clock 1.5 GHz vs 2.1 GHz nominal — **verdict (D), RESULTS.md §29**
Per-APU power limit under dense 64-bit VALU issue: whole APU ~1 580 MHz on
every node and in both SPX and CPX; a lone CPX partition (38 CU) reaches
1 956 MHz. `clock64()` tracks sclk. Aggregate throughput and HBM bandwidth
identical in SPX and CPX.
- [x] `22_clock` on PPAC SPX (§24), SH5 SPX and SH5 CPX (§29)
- [~] **ARCHIVED 2026-09-12** — `ppac-pl1-s25-40` (4-APU CPX, 24 devices)
      has been `DOWN+NOT_RESPONDING` since 2026-08-04 and may not return for
      some time. Not a gate for anything; nothing waits on it. If it ever
      shows `idle` in `sinfo -p PPAC_MI300A_CPX`: run `nodecheck.sh`, `22_clock`, `02_capacity 4`, and
      `17_hostreg 4` to see whether 24-way partitioning changes the power
      story, the NUMA layout, or host-staging bandwidth. Check
      `sinfo -p PPAC_MI300A_CPX` at the start of each session.
- (E) `clock64()` on gfx942 may tick at a fixed reference rather than the
  shader clock (`s_memtime` semantics changed across generations), in which
  case the ratio is not sclk at all.
- (A) The node's power cap or a firmware/`amd-smi` setting; other nodes may
  differ.
- (D) Real: dense 64-bit VALU issue is current-limited on this part.
- **Tests:** (1) Cross-check `22_clock` against `amd-smi metric --clock`
  sampled at 100 ms during a 1 s burst; and against a known-rate kernel (a
  dependent `v_add_u32` chain has a fixed cycles/op). If the three agree,
  the reading is real. (2) Repeat on `ppac-pl1-s24-26` and `-30`. (3) Run a
  light FP32 kernel: if it reaches 2.1 GHz while the u64 chain does not, the
  limit is workload-dependent — (D) with mechanism. (4) Check
  `amd-smi static --limit` for the configured power/clock caps.

### D7. `hipMemcpy` D2H at 58.5 GB/s, flat — **verdict (E) — §30: 262 GB/s with 8 streams**
- (E) Our copies are from `hipMalloc` device memory into registered host
  memory on the default (null) stream, one copy per device; the blit path
  may be faster with `hipMemcpyDtoHAsync` on a created stream, with several
  concurrent copies per device, or from `hipHostMalloc` targets.
- (A) The paper's "4-thread D2H" may already use multiple streams per
  device; their 12.6 s absorbs it either way.
- (D) The SDMA engines on MI300A really are ~60 GB/s each.
- **Tests:** vary stream (null vs created), copies in flight per device
  (1, 2, 4, 8), chunk size (64 MiB … 16 GiB), and target allocator
  (`hipHostMalloc` vs registered). Report the best. If the best is still
  < 200 GB/s, (D); the kernel-store recommendation stands regardless.

### D8. CPU CRT 1.4 s per 2³¹ — 64 % of the multiply — **verdict (E) — §34**: carry window, not division; tight window 2.06×, within 16 % of the STREAM floor; FP64 Barrett is 1.8× slower on Zen4
- (E) Our Garner uses `__int128 %` three times per coefficient; the paper's
  `crt4_fast` with precomputed M₁, M₂ and x86 `divq` may be leaner, and our
  8-limb sliding window may be slower than their 256-bit accumulate.
- (A) The paper never claims the CRT is cheap; 12.6 s for 10dP is consistent
  with ~1.4 s per multiply.
- **Tests:** (1) Profile `crt_carry_par` with `perf` on the node: fraction in
  division vs multiply vs memory. (2) Replace `%` by Barrett with
  precomputed reciprocals; remeasure. (3) Check memory-boundedness: 4 planes
  × 8 B in + 8 B out = 40 B per coefficient → 2³¹ × 40 B / 194 GB/s = 0.44 s
  STREAM floor. If a tuned version approaches 0.5 s, (E) on our side and the
  Phase 5 GPU-CRT gain shrinks to ~2×.

### D9. NUMA interleave: 26–55× cliff vs "+20 s" — **verdict (A) — §30**
- (A) Fully consistent: +20 s is what the cliff costs on the fraction of
  traffic that hits host staging in their pipeline. Not a discrepancy.
- (E) Our remote-node number could be pessimised by `MPOL_BIND` forcing all
  pages remote; the paper's interleave would be ¼ local.
- **Tests:** compute the expected interleave rate from local/remote harmonic
  mean (¼ at 3.7 TB/s, ¾ at 67 GB/s → ~89 GB/s read); measured 144. Close
  enough to record (A); optionally measure with `numactl --interleave` on the
  process instead of `mbind` to match the paper's regressed configuration.

### D10. calloc vs malloc "≈ 5 s" — **verdict (A) — §30**
- (A) Pool reuse: `calloc` on a previously faulted region must zero it
  (memset at ~10 GB/s single-thread → 5 s for 50 GB). Not a discrepancy.
- **Test:** free-and-reallocate a 32 GiB region 3× with calloc vs malloc
  (`21_alloc` extension); expect the zeroing cost to appear on reuse.

### D11. Batched log L = 11: 343 vs 103 GB/s (we are 3.3× faster) — **verdict (A) — §30**
- (A) Different byte convention (D5) or the paper's batch tier includes the
  `scatter_expand_k` gather and CRT per sub-transform in its bandwidth
  figure; ours is transform-only.
- (E) Our b1 kernel at L = 2^11 runs one b16 stage with a 2-point tile —
  possibly our number is the one that is off (too *high* is unlikely to be a
  bug, but check that the batched b1 pass actually does the work: verify
  output for the batched 2^11 case, which §25 did only at 2^14).
- **Tests:** add a 2^11 batched correctness check; measure batch tier
  end-to-end (expand + NTT + CRT) at L = 2^11 for comparison with 103.

### D12. Q1 — 10dP needs a 2³² transform, pools hold 2³¹ — **verdict (D) — §35**: the paper omits a top-level split; only a 2×2 / Karatsuba split at 2³¹ is consistent with its 128 GB and 12.6 s. **Decided: (i) default, (ii) kept as `POOL_LOG=32`**
- (A) Candidates: A formed as two half-products; 10^d folded into the
  reciprocal (X = ⌊P · 10^d·μ⌋); T·P done as (5^d·P) << d with 5^d at
  1.45 × 10⁹ limbs (product 3.5 × 10⁹ limbs — still > 2³¹); or the pools
  are grown to 2³² for that one phase and the 128 GB figure is the steady
  state.
- (D) The paper's memory table is wrong.
- **Tests:** arithmetic only: for each candidate, compute peak RSS against
  the paper's 256 GB and time against 12.6 s using the B9 model. The
  candidate that fits both is the working assumption for Phase 3; record
  which. This is the deferred memory-segmentation discussion.

### D13. Two small effects in our own campaigns that did not reproduce — **verdict (E) noise — §30**
(XOR swizzle +5 %, ulong2 twiddle +6 %; RESULTS.md §24)
- (E) Best-of-3 on 3 ms bursts at a clock that wanders 1.40–1.57 GHz (§24)
  is inside the noise.
- **Tests:** best-of-20 with in-kernel clock reading per launch, report
  Gbfly per GHz. Verdict either way is cheap.

### Verdicts (RESULTS.md §29–35)
| item | verdict | one line |
|---|---|---|
| D1 modmul 1.72× | **D** | 22 VALU/modmul, 1.59 GHz; no ILP/occupancy/rounding variant reaches 775 |
| D2 0.57 % | A (partly) | needs one lazy operand; [0,4p) × canonical is 21 % wrong even with two corrections → fold first |
| D3 FP64 adds | A | paper omits that adds are integer |
| D4 twiddles | A | literal per-thread costs +13 %, per-block +9 %; table best |
| D5 NTT 0.93× | **E** | template STG + 2048-element blocks → 1.17 TB/s, 1 450 / 1 380 GB/s batched |
| D6 clock | **D** | per-APU power limit; lone CPX partition 1 956 MHz; `s25-40` check archived (node down since 2026-08-04) |
| D7 D2H 58 GB/s | **E** | 262 GB/s with 8 streams; still 7× below a kernel store |
| D8 CPU CRT | **E** | carry window, not division; tight window 2.06×; FP64 Garner 1.8× slower |
| D9 NUMA | A | consistent with the paper's +20 s |
| D10 calloc | A | glibc always mmaps this size; a pool-reuse effect |
| D11 log L = 11 | A | our kernel verified; paper counts the whole tier |
| D12 Q1 | **D** | paper omits the top-level split for > 2³¹-point products; decision (i)/(ii) pending |
| D13 ±5 % effects | E | noise |

Net for the reproduction: two of our own errors fixed (D5, D7, D8) made it
faster than the paper on every measured component; three genuine gaps in
the paper (D1, D6, D12) none of which blocks it.

---

## 7. Phase 2 — suite infrastructure and known gaps (overlaps Phase 1)

### 7.1 Infrastructure
- [x] **Machine-readable output** in `common_ntt.h`: `result()` emits
      `RESULT <bench> <metric> <unit> <node> <apu0> … <apu3>`; `meta()` emits
      `META host= rocm= kernel= sclk= mclk= date=`. `report_sum`/`report_max`
      call `result()` so all programs get it for free.
- [x] **Runner** `./suite [-w node] [-t time] [bench…]`: loads ROCm, builds, runs each
      program under `srun -w <node>`, tees to `results/<date>_<host>/<bench>.log`,
      extracts RESULT lines to `results.tsv`. `./diff A B` prints ratios.
- [x] **Telemetry**: runner samples `amd-smi metric --clock --power --temp`
      for the run's duration into the same directory.
- [x] **Extract duplicated primitives** (§2 list) into `bench/ntt_kernels.h`;
      06/07/12/13/14 use it, all re-verified (RESULTS.md §24).
- [x] **`make isa B=<bench> K=<kernel-substring>`** (`isa.py`): instruction
      classes per kernel and per innermost loop from `--save-temps` output
      (VGPR/occupancy metadata parsing still to add — the `.s` comments are
      absent at -O3 for these kernels).
- [x] **Retire history**: 04, 05, 08 → `bench/old/` (`make old`); `bench/README.md`
      maps each program to its RESULTS.md section.
- [x] **Layout for new work**: `bench/arith/ mem/ lds/ fabric/ kernel/
      sustained/ system/` created; first programs `arith/mfma`, `mem/infcache`.

### 7.2 Known gaps in existing measurements
- [x] **Clock during bursts** (`bench/22_clock`, RESULTS.md §24): 1.47–1.60 GHz
      at every burst length, solo or all four; §9–10 cyc/op restated ×0.74.
- [x] Repeat-measure the two small effects that did not reproduce in §24:
      five runs each in §30 (D13) — swizzle +2 ± 3 %, ulong2 0; noise.
- [x] 09: `asm volatile` barriers, in-kernel clock, one-wave-per-CU
      latency (§37): add64 26 cyc, butterfly 20.3 cyc/op at 1.48 GHz.
- [x] 13: `mem/infcache` (§37) — only a 1.35× effect; the split is cheap
      because a one-line-per-lane gather still moves ~2 TB/s of lines.
- [x] R10's int8 MFMA: `arith/mfma` (§37) — 600–724 TMAC/s per APU at the
      power-limited 1.3–1.4 GHz; R10's 880 was a higher-clock burst.

---

## 8. Phase 3 — implementation of the paper's pipeline

Build faithfully first, as `~/ntt/ecalc/`, with the arithmetic engine and the
transform behind narrow interfaces so Phase 5 can swap in our alternatives.
Every module gets a GMP-backed test before the next module uses it. Scale by
decades 10⁶ → 4×10¹⁰ digits, verifying at each.

```
~/ntt/ecalc/
  bigint.h/.c     limb arrays, views, shifts, add/sub, repack (b=64), get_digit
  modarith.h      4 primes, FP64-Barrett modmul (paper), Shoup u64 (alt), canon64
  ntt.h/.c        ntt_tile_dif_b16<STG>, b1 pass, DIT inverse, scale/pw fusions
  rns_mul.h/.c    mdev, mdev_pair, batch, grpB; hstage/dev pools
  crt.h/.c        crt_carry_par4 (CPU), crt4_gpu S-stripe
  newton.h/.c     newton_recip_seeded, newton_divmod
  binsplit.h/.c   binsplit_bfs, seed spans, level pools, tier selection
  todec.h/.c      to_dec_bfs, divisor cache, prewarm, LEAF kernel
  verify.h/.c     tier-1 residue identity, tier-2 windows, hash61
  mem.h/.c        grow-only pools, pinned staging, pow2 grow, RSS sampling
  ecalc.c         driver: phases, timers, env knobs (NTT_B16_STG, PW_FUSE, NEWTON_R2TRUNC)
  tests/          one program per module, all GMP-checked
  ref/            known digits of e (generated once with GMP)
```
Makefile: add `-lgmp`; `ecalc` and `tests/*` targets.

| step | work | test (oracle) | est. |
|---|---|---|---:|
| 0 ✅ 2026-09-13 | `ref/gen_e.c`: GMP binary splitting on the CPU → `e_1e9.txt` + SHA-256 per 10⁶-digit block. `tests/harness.h`: GMP ↔ bigint, random generators (uniform, all-ones, zeros, single-bit, sparse), timing, `VERIFY` | — | ½ d |
| 1 ✅ 2026-09-13 | modarith: paper's 8-line modmul, four primes, ω₃₃; `canon64`; Shoup-u64 behind the same interface | `t_modarith`: 10⁹ random pairs/prime vs `__uint128_t`; edge values; one-correction failure count; ω orders via GMP | 1 d |
| 2 ✅ 2026-09-13 (STG 3–7; 8 not built, §33) | ntt: b16 tile kernel, b1 kernel, DIT inverse, `NTT_B16_STG`, scale-fuse, `PW_FUSE`; twiddle strategy per B2 | `t_ntt`: (1) n = 2⁴…2¹⁰ vs GMP O(n²) DFT incl. bit-reversed order; (2) n = 2¹⁰…2³¹ round trip; (3) convolution vs schoolbook and vs `mpz_mul`; (4) bit-identical across STG ∈ {5,6,7,8}; (5) TB/s per pass vs paper and bench/06 | 3 d |
| 3 ✅ 2026-09-13 (RESULTS §38) | mem then rns_mul: pinned hstage (malloc + `hipHostRegister`, NUMA-pinned first-touch), grow-only pools sized by `POOL_LOG` (default 31); mdev, mdev_pair, batch (`scatter_expand_k`, prefix-sum offsets, zero-extend), grpB; **`rns_multiply_split`: Karatsuba over halves for any product > 2^POOL_LOG points** (Q1 decision, §35) | `t_mul`: every tier at sizes crossing 2¹⁷, 2¹⁸, 2²⁷, 2²⁸ limbs; batch M from 1 to budget cap; grpB with 1, 8, 9 unique B; vs `mpz_mul`; hash61 at full size; measure tier crossovers on this node | 4 d |
| 4 ✅ 2026-09-13 (RESULTS §38) | crt: CPU Garner (M₁ 128-bit, M₂ 192-bit, `divq`, 96 stripes, 32-limb spill, merge); GPU S-stripe | `t_crt`: random 206-bit residues vs GMP; adversarial carries across stripe boundaries, full spill; throughput vs limbs — confirm CPU keeps up at 2³¹ (S11 cliff) | 2 d |
| 5 ✅ 2026-09-13 (RESULTS §39) | newton: `newton_recip_seeded` (r₂ truncation, overshoot shrink/retry, `R2TRUNC`); `newton_divmod` with both corrections | `t_newton`: vs `mpz_tdiv_qr` at 2¹⁰…2²⁸ limbs; Q = 2ᵏ, 2ᵏ−1; A near multiples of Q; count corrections; 0 ≤ R < Q; force the overshoot path with a perturbed seed | 2 d |
| 6 ✅ 2026-09-13 (RESULTS §39) | binsplit: seed spans (OpenMP schoolbook), level loop, four-way tier selection, level pools | `t_bs`: P, Q vs GMP recursion to 10⁷ terms with thresholds lowered via env so every tier runs; e to 10⁶, 10⁷ end-to-end through GMP division + `mpz_get_str` | 3 d |
| 7 ✅ 2026-09-13 (RESULTS §39; prewarm unseeded) | todec: divisor cache, bottom-up prewarm (T-square, μ-square seed + one doubling), TOP/LEAF/DEEP/MID, LEAF 10¹⁸ Barrett kernel | `t_dec`: vs `mpz_get_str` at 10⁶…10⁹; LEAF vs `mpz_tdiv_qr_ui` on 10⁸ limbs; leading zeros in every piece; cache μ vs GMP 2^(n+k)/T | 3 d |
| 8 ✅ 2026-09-13 (RESULTS §39; e to 10⁹ identical to ref) | verify + driver: T1 (8 primes, recurrence in ℤ/q, Horner), T2 windows, six phases, per-phase timers, VmHWM sampling, `META`/`RESULT` | single-bit fault injected into X is caught by T1; T1 passes on GMP-produced (X, R) | 1 d |
| 9 ✅ 2026-09-14: **4 × 10¹⁰ verified in 281–290 s without T2 (paper 285.7), peak RSS 248 GB (paper 256); all phases within 15 %** — earlier: 337 s (paper 285.7, 1.18×), peak 355 GB; bs 0.64×, 10dP 0.72×, dc 1.01× of the paper; dm 1.58× and memory 1.39× still outside the 15 % criterion (RESULTS §39, step 9b) | scale-up in tmux inside an 8 h allocation, `amd-smi` sampled | see §9 | 1 wk |

---

### Rules learned in Phase 1 (binding for every Phase 3 kernel)
1. The FP64 modmul takes at most one lazy operand; canonicalise the other
   (§19). Two corrections each direction, always.
2. Butterfly add/sub in 64-bit integer; FP64 only inside the modmul (§25).
3. Twiddles from two-level tables, never per-thread exponentiation (§25, §32:
   +13 % per-thread, +9 % per-block).
3b. Stage count compiled in (`template<int STG>`), and every block holds
   2048 elements whatever the tile size; `b1` stays at 2^10 (§33).
4. Host staging: malloc + `hipHostRegister`, first-touched by threads pinned
   to the APU's NUMA node; never interleave (§20).
5. Return results from device to host with a kernel store, not `hipMemcpy`
   (§26, §30) — 7× even against an 8-stream blit; and fold lazy values to
   [0, 2p) *before* every multiply (§30 D2).
6. Report rates; derive cycles only from in-kernel clock readings (§24).
7. CPU CRT: `__int128 %` Garner with a 3-limb + carry-bit window (§34);
   never FP64 Barrett on the CPU, never a wide sliding window.

## 9. Phase 4 — verification and acceptance ✅ 2026-09-14 (RESULTS.md §40: all tests pass, 4 × 10¹⁰ verified in 281–290 s vs 285.7, peak RSS 248 GB vs 256; **every acceptance criterion met**)

| level | what | oracle |
|---|---|---|
| unit | modmul, canon64, ω orders, μ constants | `__uint128_t`, GMP |
| unit | NTT fwd (small n), round trip (all n), pass splits | GMP DFT, identity |
| unit | multiply tiers at every boundary | `mpz_mul`, hash61 |
| unit | CRT + carry, adversarial carries | GMP |
| unit | Newton recip/divmod, LEAF Barrett | `mpz_tdiv_qr`, `mpz_tdiv_qr_ui` |
| integration | bs P, Q; dc digits | GMP recursion, `mpz_get_str` |
| end-to-end | e to 10⁶ … 10⁹ | `ref/e_1e9.txt` |
| end-to-end | e to 10¹⁰, 4×10¹⁰ | T1 residue identity, T2 windows vs published digits, fault injection |
| performance | per-phase times, NTT TB/s, peak RSS | Table I, §VII, §VI |

Scale-up ladder:
| digits | expected wall | check |
|---:|---:|---|
| 10⁶ | < 1 s | all digits vs GMP |
| 10⁷ | seconds | same |
| 10⁸ | ~1 s | same |
| 10⁹ | ~7 s | all digits vs `ref/e_1e9.txt`; T1 |
| 10¹⁰ | ~70 s | T1 + windows; peak RSS |
| 4×10¹⁰ | ~286 s | T1 + T2; per-phase vs Table I; peak RSS vs 256 GB |

**Acceptance for "reproduced":** all digits agree at 10⁹; T1 passes and T2
windows match at 4×10¹⁰; each phase within ~15 % of the paper; peak RSS ≤
256 GB. Record as RESULTS.md campaign 4.

---

## 10. Phase 5 — experiments: the paper's structure vs ours ✅ 2026-09-15 (all six items measured or dropped by analysis: RESULTS §41–47; the paper's structure and arithmetic win on this node; ceiling 5 × 10¹⁰ digits) (RESULTS.md §41: Q1 (ii) measured — 2.6× on the one 2³² product but 256 GiB of device pools, (i) stands; Q2 dropped — b = 48/3 primes changes neither memory nor compute; item 1 analysed; items 3–6 need builds, item 5 first)

Each is one switch behind the Phase 3 interfaces, measured against the
faithful baseline on the same node:

1. ~~**Shoup u64 modmul** for FP64 Barrett~~ ✅ 2026-09-15 measured in the b1 pass: 2–3 % slower, bit-identical; FP64 stays (RESULTS §47).
2. ~~**b = 48 with 3 primes** (Q2): 25 % less plane memory~~ **dropped** — the same bytes and the same transform work as 4 primes at b = 64 (RESULTS §41).
3. ~~**Register-blocked rows (06/12) as the b16 body**~~ ✅ 2026-09-14: `NTT_B16_BODY=1`, bit-identical, +19 % forward / +15 % inverse at 2³¹, ≈ 2 % on the pipeline (RESULTS §43).
4. ~~**One-prime-per-device vs four-step corner turn**~~ dropped 2026-09-15: the same device memory per product whatever the distribution (RESULTS §46); four-step stays the multi-node path.
5. ~~**2 × 62-bit primes (S3)** in the paper's pipeline~~ ✅ 2026-09-15 built and verified (`RNS_ENGINE=2`): 2.2× slower, higher peak; the density argument does not apply to a host-resident pipeline (RESULTS §44, CORRECTION to ALGORITHM.md). Engine 1 stays.
6. Items the paper lists as future work: ~~radix-4 stages in the b16 kernel~~ ✅ measured — no gain once the body is register-blocked (RESULTS §43); negacyclic NTT for Karp-Markstein — deferred (the pipeline's Newton does not use Karp–Markstein).

Record CORRECTIONs to ALGORITHM.md wherever the paper's structure wins.

---

## 11. Phase 6 — broader MI300A capability benchmarks ✅ 2026-09-15 complete (RESULTS §48–49: nine programs across the six groups; 7-smooth lengths answered by analysis — 37 % padding in dc, ≈ 11 s, needs a radix-3 pass)

Ordered by relevance to this project; each lands in its `bench/<group>/`.

- **system/**: launch latency, event cost, stream concurrency; allocator
  curves (B8); NUMA placement and CPU↔GPU coherence (`hostc` vs `host`).
- **mem/**: L2 and Infinity Cache BW/capacity (working-set sweep 1 MB → 4 GB),
  pointer-chase latency; TLB reach on a 460 GiB pinned arena; page sizes;
  gather/scatter and global atomics; HBM stride/width sweep.
- **fabric/**: per-link point-to-point BW/latency, uni- and bidirectional;
  push into host-pinned targets at 64-bit; two concurrent push kernels per
  APU; remote atomics.
- **arith/**: occupancy/register sweeps (threads/block, pts/thread,
  `launch_bounds`); a real 32-bit engine; FP32, packed FP16/BF16, MFMA
  bf16/FP64 for the record.
- **lds/**: throughput vs bytes/block (occupancy); `ds_read_b96/b128` with
  swizzle; `ds_swizzle` and DPP vs `ds_bpermute`.
- **kernel/**: N sweep 512–4096 for the row transform (R11's N = 512
  hypothesis); radix-4/8 in register groups; variable 7-smooth L
  (ALGORITHM.md Tier 1 item 1).

---

## 12. Conventions for every benchmark and test

- Header comment: the question, why it matters, usage line.
- Verify when there is a right answer; print `VERIFY … OK/FAILED`; exit
  non-zero on failure.
- Measure concurrently on all APUs with the OpenMP-barrier pattern; report
  per-APU and node — `report_sum` for throughput, `report_max` for wall-clock.
- Best-of-N (N ≥ 3) for bursts; per-window for sustained.
- Emit `RESULT` and `META` lines; run through `./suite` so logs land in
  `results/`.
- Pin one node for a campaign; name it in RESULTS.md.
- Record the verdict in `RESULTS.md` under a numbered section; mark any
  DESIGN.md / ALGORITHM.md claim it overturns as CORRECTION.
- Keep copies off aac6 (`~/apucode/ntt` on the laptop): the home is not
  backed up.

---

## 13. Open decisions

| # | decision | decided by |
|---|---|---|
| Q1 | ~~how 10dP fits the 2³¹ pools~~ **decided 2026-09-12: (i) 2³¹ pools + Karatsuba top-level split is the default; (ii) 2³² pools kept as a build/runtime switch (`POOL_LOG=32`) in case a more detailed follow-up paper describes the actual method** | §35 |
| Q2 | b = 48 / 3 primes | Phase 5 item 2 |
| Q3 | ~~compute- or memory-bound~~ **resolved** (RESULTS.md §25): LDS/modmul-bound at 1.0 TB/s, 27 % of HBM | B2 |
| Q4 | ~~correction count in the FP64 modmul~~ **resolved** (§19, §30): two needed for lazy × canonical; both-lazy or [0,4p) is unsafe; fold before every multiply | B1, D2 |
| — | CPU vs GPU CRT for mdev at 2³¹ limbs: CPU CRT ≈ 0.6 s with the tight carry (§34); multiply ≈ 1.1 s, CRT + D2H ≈ 60 %; GPU peer-gather CRT would gain ≈ 2× — Phase 5 | B5, B6, B9, D8 |
| — | ~~twiddle: per-thread exponentiation vs table~~ **table** (2 × 4096 two-level, one modmul); exponentiation halves the speed (§25) | B2 |
| — | ~~ROCm 7.2.4 vs 6.4.3~~ not needed, B2 within 7 % | B2 |
| — | does the 4-APU CPX node (`ppac-pl1-s25-40`) change clocks, NUMA or staging bandwidth? | **archived** — node down since 2026-08-04; opportunistic only, not a dependency |
| — | per-APU spread of up to 20 % within one run (§30 D13): per-APU power/clock? | measure per-APU sclk with `22_clock` during 06/07 |

---

## 14. Status log

| date | item | note |
|---|---|---|
| 2026-09-18 | **Grid split of the device products** (§66): pieces chosen for the fewest plane points instead of halving the longer operand — decimal 4 × 10¹⁰ dm 48.9 → 37.9 s, phases **108.3 ± 1.0**, wall 132.0 ± 3.1, 160 GB; binary 165.4 / 187.9 / 233 GB; digits identical, three runs each. The 3·2³⁰ plane pool is no longer needed. WP6 merged (§65). `NEWTON_DEVICE` and `BS_DEV_MDEV` default on; all six switch combinations verified at 10⁹ | RESULTS.md §65–66 |
| 2026-09-18 | **Final variance, everything on device** (§62b): decimal 4 × 10¹⁰ phases **119.2 ± 1.7 s**, wall 142.0 ± 1.8, peak host 160 GB; binary 166.6 ± 1.4 / 189.1 ± 1.6 / 233 GB; ten runs VERIFY OK. Top bs levels on the device tier adopted as default (`BS_DEV_MDEV=1`, §64) after the coalescing block pool; the decimal CPU-phase spread of §62 is gone with the host pools. §63 and the HTML report updated | RESULTS.md §62b–64 |
| 2026-09-18 | **WP7 (checkpoint/restart) done** (§61, `results/WP7.md`): per-level bs snapshots, restart bit-identical at 10⁸/10⁹ both bases (15/15 cmp), ≈ 1 GB/s; merged | RESULTS.md §61 |
| 2026-09-18 | **WP5 single-node cell complete** (§59): device bigint, distributed tier on real APUs, dm on device-resident numbers — 4 × 10¹⁰ verified in both bases: binary phases 187 → 175 s, decimal 138 → 130.5 s; dm-phase host RSS 155 → 80 GB. Multi-node pieces (TCP correctness) with the WP6 agent | RESULTS.md §59 |
| 2026-09-18 | Autonomous 8-hour session started: WP5 step 3 (device-resident dm) being brought to 4 × 10¹⁰; WP6 (TCP multi-node correctness) and WP7 (bs checkpoint/restart) delegated to agents in isolated worktrees (`results/WP6.md`, `results/WP7.md`), to be merged and recorded here; then five-run variance per base on the final code and the final comparison | — |
| 2026-09-17 | **WP4 done** (§58): decimal `mul_1` Barrett (2.6 ns/limb), school tier off, seed span 256 (both bases). 4 × 10¹⁰ phases: decimal **138 s** / 294 GB, binary 187 s / 246 GB, verified. The deeper decimal split (≈ 20 s) is a pool-size trade that opens with WP5. Next: WP5 | RESULTS.md §58 |
| 2026-09-17 | **WP8 done** (§57): prime set with 3·2⁴⁴ ∣ p−1 (switch), radix-3 layer on the 2ᵏ engine, 3·2ᵏ lengths in the mdev and batch tiers; all tests and digits unchanged; 4 × 10¹⁰ decimal phases 156 → 149 s, binary unchanged, no memory change (§53 projection corrected: the top products are split, not single transforms). Next: WP4 | RESULTS.md §57 |
| 2026-09-18 | **WP3 done** (§56): four device regions with subtree ownership, locality-aware batch tier (APU d does all four primes of its region), +P₂ in the CRT, lengths from a kernel, pools pregrown at init, `e_terms` bisection (−45 s of hidden wall time). 4 × 10¹⁰: binary phases 209 → 187 s, wall 271 → 214; decimal phases 193 → 156, wall 259 → 184; both verified, 10⁹ identical. Next: WP8 | RESULTS.md §56 |
| 2026-09-17 | **WP3 go**: locality, not allocation kind, is the gain (own-node reads 3.8 TB/s vs 93 GB/s remote; every all-read-all layout caps at ≈ 700 GB/s); XNACK=1 doubles the run time; CPU works on hipMalloc memory at full rate. Layout: per-APU device pools with subtree ownership (batch tier, no staging), dm numbers quartered with the 4-APU distributed transform (WP5) | RESULTS.md §55 |
| 2026-09-17 | **WP1 assessment complete** (§54): with k-anchored Newton (base-independent, binary 291 → 271 s) and the decimal column schoolbook, decimal 4 × 10¹⁰ = 259 s / 294 GB vs binary 271 s / 246 GB, digits identical in both bases; remaining decimal costs are seeds (`mul_1` division, ≈ 10 s) and the 2³³ top-level products (WP8, ≈ 25 s). Binary-base alternative sized under WP2. **Decision pending** | RESULTS.md §54 |
| 2026-09-17 | WP1 attribution items 2–3 done: the decimal recip and the 352 GB peak are one extra Newton doubling (k 3.5 % above 2³¹ vs binary 3.3 % below), last step at 2³²–2³³ points; fix = anchor the doubling sequence at k (both bases). Projection with fixes: ≈ 178 s / ≈ 270 GB vs binary 287 / 248 — user's decision | RESULTS.md §53 |
| 2026-09-17 | WP1 attribution item 1 done: seeds = 128-bit division by 10¹⁸ per inner product (4–5× per op, lazy reduction fixes it); batch tier = every level 2× NTT length from the 7 % limb growth crossing powers of two (seed-span parameter fixes it); decimal CRT split only +6 %. Lead for items 2–3: one extra Newton doubling (precision crosses 2²⁹ at 10¹⁰, 2³¹ at 4 × 10¹⁰) | RESULTS.md §52 |
| 2026-09-17 | **WP1 gate complete**: decimal identical to 10⁹; 4 × 10¹⁰ verified, five runs 323 ± 10 s vs 291 binary, peak 352 vs 248 GB (bs +38 %, recip 2.7×; 10dP+dc −85 s). OOM cause was the binary-formula prewarm length (fixed). WP5 `t_dist` OK (65), WP6 `t_comm` OK on the node. **Awaiting the user's decision on WP1** | RESULTS.md §50–51 |
| 2026-09-16 | WP5 code written while the WP1 gate ran (`ntt_dist`, `comm_sim4`, `t_dist`; untested — aac6 unreachable from littleblue tonight). **Layout finding:** doing the local pass first forces the point convention m = i + R·j, i.e. block-cyclic limb ownership (runs of R/size limbs); contiguous ownership would cost two all-to-alls per transform instead of one. Recorded in `ntt_dist.h`; decision for the user with the WP5 data | RESULTS.md §51 (pending) |
| 2026-09-16 | WP1 in progress on `wp1-decimal-base`: decimal base implemented; both bases pass all tests; decimal 10⁸/10⁹ byte-identical; WP2's phase removals fall out of the switch. Gate running | RESULTS.md §50 |
| 2026-09-15 | **Phase 7 planned** (§15): the single-node cell of multi-node design A — decimal base as a switch, device-resident pools, rank abstraction, inter-node correctness on aac6, radix-3 last; 8 WPs, ≈ 25 days; every change adopted by the user's decision on measured data; repository github.com/cecoppola/ntt | — |
| 2026-09-15 | **Phase 6 complete**: system/alloc, mem/stride, lds/xchg, kernel/rowN; 7-smooth analysis (dc pads 37 % of points; 3·2ᵏ lengths would save ≈ 11 s) | RESULTS.md §49 |
| 2026-09-15 | **Phase 5 sequence complete (tasks 1–6)**; ALGORITHM.md Part 6 CORRECTIONs; campaign report artifact https://claude.ai/code/artifact/928ec61d-c221-402f-81a8-5341c427b483 | — |
| 2026-09-15 | Phase 5 item 1 measured (Shoup b1: −2 %), Phase 5 closed; Phase 6 first pass: five group benchmarks | RESULTS.md §47–48 |
| 2026-09-15 | **Past the paper: e to 5 × 10¹⁰ digits verified in 442 s, peak 375 GB** — the node's ceiling for this pipeline (RESULTS §45); item 4 dropped by analysis (§46) | — |
| 2026-09-15 | Phase 5 item 5: engine 2 (2 × 62-bit, 45-bit points) built, verified to 4 × 10¹⁰; 641 s / 282 GB vs 291 s / 248 GB — density does not help a host-resident pipeline; engine 1 stays | RESULTS.md §44 |
| 2026-09-14 | Phase 5 tasks 2–3: register-blocked b16 body (+19 %/+15 % at 2³¹, bit-identical, pipeline 285.5 s) and radix-4 stages (no gain) | RESULTS.md §43 |
| 2026-09-14 | Phase 5 task 1 (variance baseline): five 4 × 10¹⁰ runs, 291.2 ± 3.5 s (1.2 %), phases 0.3–3 %, no per-APU asymmetry | RESULTS.md §42 |
| 2026-09-14 | Phase 5 started: Q1 (ii) measured (2.68 vs 6.88 s, 256 GiB pools → (i) stands), Q2 dropped by arithmetic, item 1 analysed; items 3–6 scoped (item 5, 2 × 62-bit primes, first) | RESULTS.md §41 |
| 2026-09-14 | **Phase 4 exceptions closed**: high-half A·μ and low-product X·Q in the division (dm 48 s vs 46.8), lazy allocation and scratch release in dc (peak 248 GB vs 256); 4 × 10¹⁰ at 0.98–1.01× the paper's time, verified | RESULTS.md §40 addendum |
| 2026-09-14 | **Phase 4 complete**: `ecalc/accept.sh` sweep — every test OK, 10⁶–10⁹ identical to ref, 10¹⁰ and 4 × 10¹⁰ verified; 4 × 10¹⁰ in 317 s without T2 (1.11× paper), bs/10dP/dc within or under 15 %, dm 1.48×, RSS 350 GB | RESULTS.md §40 |
| 2026-09-14 | Step 9b pass 3: T2 26 → 4.6 s; correction-form Newton (fewer products, no measurable gain at 4 × 10¹⁰); 351 s without T2 — noise-level vs pass 2 | RESULTS.md §39 pass 3 |
| 2026-09-14 | **Step 9b tuning pass 2**: parallel shifts/copies, DEEP scratch sized once, host loops parallel → 4 × 10¹⁰ in 337 s (without T2), verified; dm (74 vs 47) and RSS (355 vs 256 GB) remain | RESULTS.md §39 pass 2 |
| 2026-09-14 | **Step 9b tuning pass 1**: striped + coalesced GPU CRT (batch and mdev), batch limit 2³⁰, seeded prewarm, windowed dc remainder → 4 × 10¹⁰ in 456 s (was 1 021), verified. Step 4 (truncated Newton) judged unsound as printed; a middle product is the route | RESULTS.md §39 step 9b |
| 2026-09-13 | **Step 9 first run: e to 4 × 10¹⁰ digits verified (T1, T2, digit residues) in 1 021 s vs paper 285.7; peak RSS 362 GB.** Gap: dc MID 301 s, bs mdev levels 177 s, reciprocal 86 s, prewarm 68 s | RESULTS.md §39 |
| 2026-09-13 | **Phase 3 steps 5–8 complete**: `newton.c` (696 checks, reciprocal error 0), `binsplit.c`, `todec.c` (10⁹ digits in 18 s vs GMP 277 s), `verify.c` (T1 + digits residue, T2, fault injection), `ecalc` driver; **e to 10⁹ digits byte-identical to `ref/`, T1/T2 pass, 31 s compute**. Remaining: step 9 scale-up (10¹⁰ running; 4 × 10¹⁰ needs an 8 h allocation), estimate ≈ 490 s vs paper 285.7 — gaps: batch product throughput (~1.1 Gpoint/s), unseeded divisor prewarm | RESULTS.md §39 |
| 2026-09-13 | **Phase 3 steps 0–4 complete** (`ecalc/`): `ref/e_1e9.txt` (442 s), `modarith.h` (125 checks), `ntt.c` (452 checks, 1.20 TB/s at 2³¹), `rns_mul.c` all tiers incl. batch GPU CRT and grpB (95 + 14 checks), `crt.c` (24 checks); 10dP-size product 9.1 s via Q1 (i) | RESULTS.md §38. CRT is compute-bound at 0.4–0.65 s/2³⁰ and 40–55 % of a multiply; GPU CRT for mdev is the Phase 5 lever |
| 2026-09-12 | suite plan written | first PLAN.md |
| 2026-09-12 | paper reviewed, EPAPER_PLAN.md written | Q1–Q4 raised |
| 2026-09-12 | plans merged into this file | EPAPER_PLAN.md removed |
| 2026-09-12 | **Phase 2 complete**: 04/05/08 retired, category layout, 09 restated (opaque chains, measured clock, true latency), `mem/infcache`, `arith/mfma` | RESULTS.md §37 |
| 2026-09-12 | `s25-40` CPX test archived (node down since 2026-08-04); Phases 0, 1, 1b closed | — |
| 2026-09-12 | **Q1 decided**: (i) 2³¹ pools + Karatsuba split by default; (ii) 2³² pools as a switch for a possible follow-up paper | Phase 3 step 3 unblocked |
| 2026-09-12 | documentation revised: PLAN §2, §3, §6.3 verdict table, §13; RESULTS §36 summary; `bench/README.md` | `s25-40` still down (since 2026-08-04) |
| 2026-09-12 | D12 decided (D): paper omits the top-level split for > 2³¹-point products; (b′) Karatsuba at 2³¹ pools is the only reading consistent with 128 GB + 12.6 s; Phase 3 choice (i) split / (ii) 2³² pools pending | RESULTS.md §35 |
| 2026-09-12 | D8 decided (E): carry loop was 40 % of the CRT; tight window 2.06× (0.29 s / 2³⁰); FP64 Garner 1.8× slower; B9 multiply ≈ 1.1 s | RESULTS.md §34 |
| 2026-09-12 | D5 decided (E): template kernels + full blocks; 2³¹ in 117 ms = 1.17 TB/s (paper 1.08); batched 1 450 / 1 380 / 1 520 GB/s at log L = 14 / 17 / 20 | RESULTS.md §33 |
| 2026-09-12 | D4 decided (A): table 137 ms, per-thread dpow +13 %, per-block dpow +9 % at 2³¹ | RESULTS.md §32 |
| 2026-09-12 | D1 decided (D): `make isa` + `isa.py`; 22 VALU/modmul, 1.59 GHz in-kernel, 1.26 IPC (cndmask co-issue); chain/occupancy/rint variants all ≥ 895 | RESULTS.md §31 |
| 2026-09-12 | documentation revised to this state (PLAN §1–4, §6, §13; RESULTS §17–30; `bench/README.md`) | — |
| 2026-09-12 | Phase 1b short items: D2, D3, D7, D9, D10, D11, D13 decided | RESULTS.md §30. D7 was our error (single stream): blit reaches 262 GB/s/APU with 8 streams, still 7× below a kernel store. Open: D1, D4, D5, D8, D12, s25-40 |
| 2026-09-12 | Phase 1b: node partition survey; D6 decided (D) | all 4-APU nodes SPX/NPS1 and identical; CPX exists only on 1-APU SH5 nodes; mode explains no discrepancy; lone CPX partition clocks 1 956 MHz → power-limited |
| 2026-09-12 | Phase 1b plan written (§6.3): 13 discrepancies, each with D/E/A hypotheses and separating tests | — |
| 2026-09-12 | **Phase 1 complete**: B2, B4, B6, B9 | RESULTS.md §25–28. Tiled DIF 1.0 TB/s at 2³¹ (paper 1.08); host-staging kernels 0.6–1.8 TB/s, kernel store 31× D2H; CPU CRT 1.4 s at 2³¹, no GPU interference; model: mdev multiply ≈ 2.2 s, 77 % CRT + D2H |
| 2026-09-12 | Blocks 1–3 closed out | 06/07/12 on `ntt_kernels.h`; 13 emits RESULT; `22_clock` (sclk 1.47–1.60 GHz under VALU load → cyc/op ×0.74, RESULTS.md §24); full default set re-run and verified as the campaign-4 baseline |
| 2026-09-12 | Block 3: gates B1, B3, B5, B7, B8 | RESULTS.md §19–23. All pass. Modmul 1.72× paper; registered memory HBM-speed if NUMA-local, 55× slower if not; D2H blit 58 GB/s; peer gather 154 GB/s/APU; power cap 550 W → 1.49 GHz under load (CORRECTION to cyc/op) |
| 2026-09-12 | Phase 2.1 harness: `result()`/`meta()`, `./suite`, `./diff`, telemetry, `ntt_kernels.h` | 13 and 14 re-verified: 1.08 ms/multiply, 2 241 / 2 339 Gbfly/s. Suite output in `results/<date>_<node>/`. Superseded 04/05/08 still build but are not in the default set |
| 2026-09-12 | Phase 0 complete | RESULTS.md §17. `ulimit -l` = 31.4 GiB flagged for B3; `amdttm.pages_limit` is 512 GiB not 96 GiB; all paper constants verified; 8 tier-1 primes proposed |

---

## 15. Phase 7 — the single-node cell of the multi-node design (2026-09-15)

Goal: rebuild the single node as one cell of design A (one prime per APU,
RNS grouping by APU slot, limbs resident, planes transient) so that scaling
to 2 048 nodes adds only a communicator. Every work package (WP) keeps the
existing GMP-checked tests green and ends with `accept.sh` plus a five-run
`variance.sh` so that each change carries its correctness result and its
per-phase times against the previous WP's mean (the numbers to beat at the
start are Phase 4's: 4 × 10¹⁰ in 289 ± 3.5 s, 248 GB). **Each change is
adopted or reverted by the user's decision on that data**; the gates below
say what is measured, not who decides. Every change lands behind a switch
or a git commit so it can be reverted alone.

Sequence rationale: WP1 changes the data representation everywhere and
must come first, as a switch; WP2 removes the phases the representation
made unnecessary; WP3 restructures memory on top of a working decimal
pipeline; WP4 is compute tuning on the final layout; WP5 adds the rank
abstraction when the cell is final; WP6 checks the inter-node code on aac6;
WP7 is verification and hardening throughout; WP8 (radix-3) comes after all
other changes. The tree is under git (github.com/cecoppola/ntt) with a tag at
the Phase 4 accepted state before WP1 begins.

| WP | items (grouped) | work | gate |
|---|---|---|---|
| **WP1 — decimal base** (D1) | **the base is a switch** (`LIMB_BASE` ∈ {2⁶⁴, 10¹⁸}): the paper-faithful binary path stays alive as the regression oracle and every test runs in both modes against GMP. First task: an **audit of every bit-level operation** — limb add/sub carry at 10¹⁸ instead of 2⁶⁴, Knuth-D schoolbook division in base 10¹⁸, `bi_shl/bi_shr` by bits (the seed, the reciprocal scaling), Newton's overshoot shrink `r ≫ 4` (becomes a division by 16), the T1 Horner (10¹⁸ instead of 2⁶⁴); limb-granular shifts are unchanged. Then base-10¹⁸ limbs in `bigint` (multiply-by-10ᵏ as a limb shift, limb-wise normalisation), seed spans producing decimal limbs, `ntt_load` from 60-bit decimal points, **decimal carry in the striped GPU CRT and the CPU CRT** (Barrett by 10¹⁸ per coefficient, from the LEAF kernel), `rns_mul_low` and `rns_mul_split` over decimal limbs (Karatsuba is base-agnostic), Newton unchanged in structure (limb shifts are decimal), T1 residues of decimal limbs, prime set re-checked for 2·60 + log₂n ≤ 206 | 5 d | `t_mul` and `t_newton` vs GMP in both bases (new generators); e to 10⁹ from `ref/` byte-identical in both bases |
| **WP2 — phases removed by WP1** | delete 10dP (A = (P+Q) shifted by d/18 limbs) and dc (X's limbs are the digit blocks; print pads leading zeros); T2 windows and digit residues read straight from limbs (T1 residues chunk-local) | 1 d | 10⁶–10⁹ identical; 10¹⁰ and 4 × 10¹⁰ verified; expected ≈ 285 → ≈ 195 s |

Sizing of the alternative (binary base kept, 2026-09-17, for the WP1
decision): with binary limbs the multi-node pipeline must also contain
**10dP** (one more full-size distributed product: +9 s single-node
compute, +1 product's worth of all-to-alls) and a **distributed dc**. dc is
the binary-splitting tree in reverse: 24 levels, of which the top ≈ 13
(products larger than a rank) are distributed Newton divisions by
10^(d/2ˡ) — each level touches all n limbs (2 products + a reciprocal, the
reciprocal precomputable per level), so the summed size is ≈ 13 n limbs,
≈ the whole bs tree again. Cost relative to the decimal design: compute
+ ≈ 30 % (dc is 82 s of the 287 s single-node run); communication ≈ 2×
(≈ 12.5 → ≈ 25 s of network per 4 × 10¹⁰-equivalent on the target fabric);
memory: no new peak (the top level is a dm-sized division). Engineering:
a rank-partitioned division tree (the mirror of WP5's bs partition) with
per-level reciprocal prewarm, leaf conversion and T2 verification — ≈ 6–8
days on top of the plan, plus the same correctness testing as WP6. With
decimal limbs none of this exists (dc 4.4 s, local formatting).
| **WP3 — everything on device** (D2) | level pools in HBM split by product (scatter reads local + peer), the P₂ add and level normalisation as kernels, `POOL_LOG` a run-time function of the digit target (2³² at ≥ 10¹⁰), mdev operands read from HBM (no repack; staging buffers retired except for host-resident bigints, which become the *only* host memory), the batch tier's planes double-buffered so the CRT of one tile overlaps the next tile's transforms (not the 2³² pools — those are the planes); `t_mul`/`t_bs`/`t_dec`-style tests on device pools | 4 d | all tests; host peak ≈ 110 GB at 4 × 10¹⁰; bs ≈ 72 → ≈ 55 s; dm without splits |
| **WP4 — compute tuning on the final layout** | seed spans as a batched GPU level (2¹⁰-point products), register-blocked body for batch products with log L ≥ 17, operand reuse extended to dm (fwd(Q) for the reciprocal's top step and X·Q; fwd(r) for r·d) | 2 d | `t_ntt` bit-identical where applicable, GMP elsewhere; 4 × 10¹⁰ ≈ 80 s |
| **WP5 — the rank abstraction** (D3, D5, GPU-direct, slabs) | a communicator interface (rank, size, all-to-all of slabs, barrier) with two implementations: single-rank identity and a synthetic four-rank layout inside one APU's HBM; the four-step distributed transform (local column pass → slab transpose → twiddle → local row pass) written against it, slab-pipelined, sourced from HBM; tree partition by rank (subtree per rank, top levels distributed); the same transform validated bit-for-bit against `ntt_fwd` on one rank and against GMP on four synthetic ranks | 5 d | `t_ntt` extended with the distributed path; the whole pipeline runs unchanged through the one-rank communicator |
| **WP6 — inter-node correctness on aac6** | aac6 has no high-speed fabric (one 1 Gb/s Intel I210 NIC per node, no IB/Slingshot device, no MPI/RCCL/UCX modules) and 3 usable 4-APU nodes (PPAC_MI300A_SPX: s24-16/26/30; s24-35 down; no per-user node cap, 8 h). So this WP is **correctness only, never performance**: a TCP-socket implementation of the WP5 communicator (an OpenMPI built in `$HOME` with the TCP BTL is the alternative), the pipeline run on 2 and 3 nodes (8 and 12 APU ranks), the distributed four-step transform and the rank-partitioned tree checked bit-for-bit against the one-rank run and against GMP, slab pipelining, checkpoint/restart from a per-level checkpoint; the SH5 single-APU nodes as extra ranks if a job may span partitions. Bandwidth, all-to-all efficiency, incast and GPU-direct RDMA are **not** measured here — they belong to the target Slingshot system (2 × 400 Gb/s per APU); the A-vs-E decision waits for a 2 048-endpoint all-to-all test there, with E kept behind a switch | 3 d | 2- and 3-node runs verified (T1, T2, 10⁹ identical); restart from a checkpoint reproduces the run |
| **WP7 — verification and hardening** | independent second run through engine 2 (2 × 62-bit primes, binary base — a separate pipeline path, verified to 5 × 10¹⁰) compared at windows, checkpoints of P, Q per tree level, `accept.sh` and `variance.sh` extended to the new phases, RESULTS.md sections per WP, ALGORITHM.md Part 7 | 2 d, spread across WPs | five-run variance ≤ 2 %; documentation current |

| **WP8 — radix-3 lengths (last, optional)** | 3·2ᵏ transform lengths for dm's A·μ products (37 % padding today): a four-prime set with 2ᵏ·3 ∣ p−1 if the paper's set lacks the factor 3, a mixed-radix pass in both kernel bodies, `t_ntt` at 3·2ᵏ; the smallest gain of the set (≈ 10 s of dm at 4 × 10¹⁰) and the only item touching the bit-identical kernels — after everything else | 3 d | `t_ntt` bit-identical where applicable, GMP elsewhere |
Expected end state on one node: 4 × 10¹⁰ digits in ≈ 80 s, host ≈ 110 GB,
device ≈ 74 GiB per APU, ≈ 9 × 10¹⁰ digits per node; the code already
partitioned by rank. Expected at 2 048 nodes (design A): ≈ 2–3 min for
2 × 10¹⁴ digits with ≈ 12 s of unhidden communication; ≈ 3 × 10¹⁴ digits
possible.

Risks, in order: the decimal carry inside the striped CRT (WP1; test first
against GMP at every stripe boundary); device level pools' peer-read share
(WP3; measure before committing — if the aggregate is below the host path's
470 GB/s, keep pools on the host and take only the staging removal); the
radix-3 prime search (WP4; may need a new set); the all-to-all efficiency
(WP6 on the target system; on aac6 only correctness can be tested — 1 GbE, no RDMA).

Total ≈ 25 days of sessions, WP1–WP4 ≈ 12 of them; WP6's performance half moves to the target system; WP8 only if chosen after WP7.

### Status at the end of the autonomous session (2026-09-18) and what remains

Done and recorded: WP1 (§50–54), WP2 (falls out of WP1), WP3 (§55–56),
WP4 (§58), WP5 single-node cell (§59), WP6 (2-node TCP correctness, §65),
WP7 (§61), WP8 (§57); the top bs levels on the device tier (§64, default);
five-run variance on the final code (§62b); the final single-node
comparison and the design-choice table (§60, §63); the grid split of the
device products (§66). Final: decimal 108.3 ± 1.0 s of phases / 132 s
wall / 160 GB; binary 165.4 / 188 / 233 GB; paper design 229 / 291 /
248 GB. Everything is on `wp1-decimal-base`;
`main` still holds the Phase 4 reproduction. The decision on the base and
on merging is the user's (§63 carries the recommendation).

Remaining, in the order I would do them:
1. ~~3·2³⁰-point planes for the device tier~~ — superseded by the grid
   split (§66), which reaches the same plane-point count with no extra
   memory. Karatsuba would apply only to binary's 2 × 2 products (≈ −2.5 s).
2. Binary's peak host memory (233 GB) is now set by dc's host-resident
   tiers; if binary is chosen, dc on device numbers is the same move as the
   dm phase (§59) — irrelevant for decimal.
3. Decimal's CPU-phase variance: closed by §62b (the page-fault cost went
   with the host pools; wall sd 1.3 %). `results/bind.out` kept.
4. WP6 beyond correctness: the TCP path is for verification only; the
   fabric numbers (§63) come from the xGMI measurements and the sizing.
5. The multi-node pipeline itself (rank-partitioned tree, slab pipelining,
   per-rank checkpoints) on the target system.

