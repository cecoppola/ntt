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
| 2026-09-22 | **Phase 12 closed** (RESULTS §77): six agents merged to `main` (3524146), the standing regression **21/21** including a forced-growth stress step. The D5 race's cause named and fixed (a same-device `hipMemcpy` returns before the copy runs on this ROCm; 56/56 against 24/26); every tree level gridded over fixed planes on a schedule (576-node node peak 1170 → 354 GB, per-node ceiling 1.9 → 7.1 × 10¹⁰); the allocation study (nothing beats `hipMalloc` cold) and the larger planes adopted on the tail layout; the SHMEM transport verified in its target forms on Sandia OpenSHMEM; `estimate.py` + `docs/TARGET.md`. One integration defect caught by measurement: the new top-checkpoint default costs 113 s on a 0.31 GB/s disk → default off. 4 × 10¹⁰: **80.7 ± 1.2 s, phases 58.8** (was 81.5 / 66.0). **576 nodes: ≈ 3.9 × 10¹³ digits in ≈ 4.0 min (modelled; 5.5 × 10¹² before)** | RESULTS.md §77, results/{R,G12,I,S12,Q,W}.md |
| 2026-09-21 | **Phase 11 closed** (RESULTS §76): six agents merged to `main` (06e06f1), regression 17/17 after every merge and over SHMEM. **10¹¹ digits on one node VERIFY OK (263 s, 445 GB)**; 4 × 10¹⁰ 81.5 ± 1.4 s; 7 × 10¹⁰ 153.5 s; the SHMEM transport with per-level PE sets; the transform balanced at any group size (576); `alltoallv` throughout; the fabric and memory models; **the T1 moduli corrected (seven were composite)**; a timing-dependent leaf-level fault under forced growth localised (open). 576-node estimate: ≈ 1.2 × 10¹³ digits as merged (the top product's scratch), ≈ 4.4 × 10¹³ in ≈ 4.6 min with the top product gridded (next) | RESULTS.md §76, results/{S,L,X,P,V,M11}.md |
| 2026-09-20 | **Phase 10 closed** (RESULTS §75): five agents (G, H, M, C, T) merged to `main` (1f0b114) and re-verified with the new standing regression `ecalc/mnaccept.sh` (17/17 after every merge). Single node 4 × 10¹⁰ **83.0 ± 1.2 s** (five runs, identical) — host peak **11.7 GB** (X never on the host, seeds streamed into the arenas); 7 × 10¹⁰ 159.7 s (157.2 with `ECALC_DM_POOL=1`, left off); **8 × 10¹⁰ on one node VERIFY OK, 210.4 s, node peak 393 GB**; `alltoallv` in every transport; xGMI push −9 %; tree-level checkpoints without the barrier. Rejected on measurement: the second pipeline plane, the transform cache at size 1. Not done: C2/D1 (two idle nodes never available); the `alltoallv` consumers; E1/E2 await the user | RESULTS.md §75, results/{G,H,M,C,T}.md |
| 2026-09-19 | **Phase 9 closed** (§74): final series 4 × 10¹⁰ **86.4 ± 1.3 s / 48.8 GB** (five runs, identical); 7 × 10¹⁰ in 163.8 s at 76.5 GB; the multi-node pipeline end to end at sizes 2–4 (10¹⁰ over four node-processes) and on real nodes; paper refreshed (14 pp.). Next: M8 on the target; 4 × 10¹⁰ over two real nodes; the open items in results/A-*.md | RESULTS.md §74, ~/xetex/e40b.tex |
| 2026-09-19 | **Phase 9 executed with seven parallel agents** (§19; RESULTS §74): M4 distributed division, M5 per-node output + streamed digits, M6 per-node checkpoints, M7 allgather + slab pipelining, the grid over shares, M9 accounting + pool sizing + balanced layout, paired transforms in the batch tier — all merged and verified on `main` (38ed61b): single node **86.6 s / 48.8 GB** at 4 × 10¹⁰; the multi-node pipeline end to end at sizes 2–4 (10¹⁰ over four node-processes VERIFY OK) and on 2 real nodes. Rejected on measurement: the DPP exchange, 3·2³⁰ planes. Closing series running | RESULTS.md §74 |
| 2026-09-19 | **M3 done** (§73): the distributed top levels over node groups with a layered communicator; digits identical at sizes 1–4 on one node and on 2 and 3 real nodes (10⁶–10⁹); merged to `main`. Steps 1–4 of the sequence complete | RESULTS.md §73, results/M3.md |
| 2026-09-18 | **Steps 1–3 done**: baseline pinned 101.0 ± 0.5 s / 74.5 GB (§71), ceiling **7 × 10¹⁰ digits verified** (182.7 s, 114 GB), 5 × 10¹⁰ identical to its reference; paper refreshed (`~/xetex/e40b.tex`, 13 pp.); step 3 staging sized to the seeds: **98.5 s / 70.8 GB** (§72), region slack rejected. M3 agent in progress | RESULTS.md §71–72 |
| 2026-09-18 | **I3 done** (§70): the decimal division entirely on the device, exact-size quarters; 4 × 10¹⁰ wall **104.8 s**, peak host **74.5 GB**; found: hipMalloc cannot hide behind GPU work; quarter classes wasted 45 %. Baseline now 104.8 s / 74.5 GB (main) | RESULTS.md §70 |
| 2026-09-18 | **I2 done** (§69): seeds during init, 4 × 10¹⁰ wall 112.1 → **107.7 s**, peak host 140 GB; default. I7 dropped. Next: I3 (decimal division entirely on device) | RESULTS.md §69 |
| 2026-09-18 | **Multi-node work paused** (user decision) at M2 done, M3 not started; state and resume instructions in §17. Single-node work continues (§16 ideas: I2, I3 next) | PLAN §17 |
| 2026-09-18 | **Decision (user): the overlapped flow (112.1 s) is the default** — `ECALC_OVERLAP=1`, `main` = tag `phase8-overlap-default`. Multi-node status: structure in place (node-process, four meshes, term-range partition, transform verified over the meshes); the combine, division and output are still centralised on node 0 (M3–M5); aac6 can exercise up to 8 node-processes (2 nodes × 4) | RESULTS.md §68 |
| 2026-09-18 | **Phase 8 started**: overlap of disjoint work (§18) measured — 4 × 10¹⁰ wall 128.8 → **112.1 s** (−13 %) under clean conditions, peak host 154 GB, digits identical; three couplings found and fixed (blocking copy stream, device-wide syncs in dbig, un-faulted host buffers); page-cache pollution of measurements identified (baseline 133.7 → 128.8 with the reference file evicted). M1 + M2 built: node-process driver, four TCP meshes, term-range partition, node 0 combining — sizes 1, 2, 4 on one node identical to the reference. `ECALC_OVERLAP` default: user decision | RESULTS.md §68 |
| 2026-09-18 | **Decision (user): the decimal final version is the code** — `LIMB_BASE=10` default in `ecalc`, `wp1-decimal-base` merged to `main` (fast-forward), Phase 4 stays at tag `phase4-accepted`; write-up as a paper (`~/xetex/e40b.tex`) before further development | RESULTS.md §67 |
| 2026-09-18 | **Session close**: low product as the grid with the pieces above the window skipped (decimal dm 37.9 → 37.0); pool bug found by `t_newton` and fixed (extents never merge across hipMalloc regions); full test set green in both bases; final three-run series decimal **108.9 ± 1.1** phases / 133.7 wall / 160 GB, binary 167.1 / 191.5 / 233 GB | RESULTS.md §66 |
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

### Decision (2026-09-18)

The user chose the **decimal final version** (§63: 108.9 s of phases,
133.7 s wall, 160 GB) as the code: it is the default configuration on
`main`; the binary pipeline stays reachable with `LIMB_BASE=2` for
reproducing the paper; the Phase 4 acceptance is tag `phase4-accepted`.
Next: the paper (`~/xetex/e40b.tex`), then the multi-node work on the
target system.

### Status at the end of the autonomous session (2026-09-18) and what remains

Done and recorded: WP1 (§50–54), WP2 (falls out of WP1), WP3 (§55–56),
WP4 (§58), WP5 single-node cell (§59), WP6 (2-node TCP correctness, §65),
WP7 (§61), WP8 (§57); the top bs levels on the device tier (§64, default);
five-run variance on the final code (§62b); the final single-node
comparison and the design-choice table (§60, §63); the grid split of the
device products and the low product (§66). Final series: decimal
108.9 ± 1.1 s of phases / 134 s wall / 160 GB; binary 167.1 / 192 /
233 GB; paper design 229 / 291 / 248 GB. Everything is on `wp1-decimal-base`;
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


---

## 16. Phase 8 — the idea list (opened 2026-09-18; add, test, decide)

Baseline: `main` @ `phase7-decimal-final`, decimal, 4 × 10¹⁰: phases
108.9 ± 1.1 s (bs 61.1 = seeds 10.4 + batch 29.7 + top levels 19.1 + 1.9;
10dP 1.7; dm 37.0 = recip 14.8 + division 22.2; T1 3.7; dc 4.2; T2 1.2),
init 20.2, other 4.0, **wall 133.7 ± 1.8 s**, peak host 160 GB, device
≈ 250 GB. Each item: what, the expected gain (sized from the record), the
cost, and the status. The user decides on measured data; items land behind
a switch.

| # | idea | expected | cost | status |
|---|---|---|---|---|
| I1 | **Phase-level overlap of disjoint work** (§18): init in parallel per APU; T1's P, Q recurrence during the GPU levels; A = 10ᵈ(P+Q) and the residues during the reciprocal; Q kept on device; digit formatting + T2 + digit residue during the low product | measured **−16.7 s** (128.8 → 112.1, clean conditions) | done | **measured (§68); default = user decision** |
| I2 | Seeds computed during init's device allocations (background thread from a hook in `rns_init`); host pool for A not pre-touched | measured **−4.4 s** (112.1 → 107.7), peak host 154 → 140 GB | done | **default (§69)** |
| I3 | Decimal division entirely on device (S = P+Q on device, A implicit, window and corrections on device, residues by kernel), exact-size quarters | measured **−2.9 s** (107.7 → 104.8) and **peak host 140 → 74.5 GB** | done | **default (§70)** |
| I4 | Reciprocal warm start: μ's top from a lower-precision run, or the last doubling's product reused across the two division products (fwd(Q) computed once for X Q and the recip's Q_t r) | −3…−5 s of dm | 2 d | idea |
| I5 | Karatsuba for products whose half-sums fit a plane (binary's 2 × 2; decimal's do not) | binary only −2.5 s | 1 d | idea, low priority |
| I6 | Batch tier: operand reuse across the tree add (Q₂ transformed once for P₁Q₂ and Q₁Q₂) | ≈ −3 s (one transform in six per pair); the paired index needs power-of-two lengths, so not on the 3·2ᵏ levels | 2 d, touches the kernel index path | idea, low priority |
| I7 | Seeds on the GPU as a batched level of 2¹⁰-point products | — | — | dropped: the seeds are off the critical path after I2 |
| I8 | Decimal `mul_1` in the seeds: Montgomery-style by 10¹⁸ or two limbs per step | seeds −2…−3 s | 0.5 d | idea |
| I9 | dm: A μ's top half only through the grid (the low pieces of the 2 × 3 grid are not needed either) | −2…−3 s | 0.5 d | idea |
| I10 | 3·2³⁰-point planes for the very top products where the grid is 1 × 2 at 52 % fill (levels 23–24) | −3 s of the top levels; +60 GB device | 1 d | idea, memory trade |
| I11 | Init: pools zeroed by kernels instead of `hipMemset` serial per device; contexts built on the CPU side once | init −3 s | 0.5 d | idea (part of I1) |
| I12 | The 40 GB output write overlapped with T2 (write while checking) and `O_DIRECT` | outside the timed run | 0.5 d | idea |
| I13 | Two-prime 62-bit engine for the batch tier only (planes transient, density irrelevant): rejected end-to-end in §44 but never measured for the batch levels alone | unknown; likely none | 1 d | idea, low priority |
| I14 | Multi-node: hierarchical all-to-all (xGMI group first), slab pipelining through `dist_fwd_pre/_post` | hides most of the fabric time | see §17 | idea |
| I15 | Region pools: the level with five output nodes puts two in region 0 (40 % of the level) — one 19 GB `hipMalloc` mid-phase (≈ 1.3 s); balance the layout (split the heavy node's pair across regions, or size region 0 for it) | −1.3 s | 0.5 d | idea (§72) |
| I16 | The reciprocal's block pool at 6–7 × 10¹⁰ overflows the donated regions again (reciprocal 16 → 31 s at 7 × 10¹⁰): donate the second parity's regions earlier / size the pool from the reciprocal's scratch | −10…−15 s at 7 × 10¹⁰, nothing at 4 × 10¹⁰ | 0.5 d | idea (§71) |

## 17. Phase 8 — the work to run on 2 048 nodes, and the form the code takes now

**Resumed 2026-09-18 (step 4 of the agreed sequence): M3 done and merged
(RESULTS §73, `results/M3.md`).** State before that, all on `main`
(tag `phase8-overlap-default`): M1 and M2 done and verified — the
node-process driver (`ecalc/mn.c`, `mn.h`), four TCP meshes over the
nodes with a start-up self-test, `mnrun.sh <procs> <cmd>`, the term-range
partition (`bs_a0/bs_b1`), node 0 gathering and combining P_r, Q_r with
its own tier; digits identical at sizes 1, 2, 4 on one node (10⁸, 10⁹).
M3–M7 and M9 done (2026-09-19, Phase 9 §19, RESULTS §74): the
distributed tree, division, per-node output and verification,
checkpoints, allgather and slab pipelining, the grid over shares, memory
accounting — the multi-node pipeline runs end to end at sizes 2–4 on one
node and on 2 real nodes, digits identical. Remaining: M8 (the RDMA
communicator, on the target system), 4 × 10¹⁰ over two real nodes (memory:
needs two nodes idle at once), the open items in results/A-*.md (an
all-to-all-v for the padded slabs, per-level TCP meshes, load balance at
non-power-of-two sizes, the leaf's regions donated before the tree at
size > 1). To resume: read this section and
RESULTS §68's last paragraph, run `SLURM_JOB_ID=<id> ./mnrun.sh 2 env
POOL_LOG=27 ./ecalc 100000000 /tmp/e.txt` to confirm the scaffolding
still works, then start M3. aac6 offers up to 3–4 real nodes in
`PPAC_MI300A_SPX` (plus `SH5_MI300A_*`, untried) and 4 node-processes
per node for logic tests. `size` 1 is untouched by any of this.

**Rank model (decided 2026-09-18): a process is a node group of g APUs**
(g = 4 on the target), driving its APUs with threads and the xGMI push
kernel as the single-node pipeline does today, with the host-side numbers
shared inside the process. `size` counts processes; the communicator is
layered — the intra-process exchange over xGMI plus an inter-process
exchange (TCP on aac6, RDMA on the target) — so a transform's all-to-all
is hierarchical from the start (M7's structure). g stays 4 in the code
(primes = devices = quarters = regions everywhere); for testing on one
aac6 node **several node-processes share the node**, each driving all
four APUs with a smaller memory share (as WP6's 8-rank `t_dist` did): sizes
1, 2, 4 on one node exercise every partitioned code path (timing
meaningless, as on the 1 GbE fabric anyway), and 2 × 1 on two real nodes
when free. The alternative — one process per APU, as first written
here and in the paper — was rejected: it gives up the shared host memory
and puts sockets between the APUs of one node. The paper's §10 is to be
corrected accordingly. The numbers are verified at every size against the
single-process run. Order and estimates (sessions of work):

| step | what | test on aac6 | est. |
|---|---|---|---|
| M1 | **The driver as a node-process**: `ecalc` reads `COMM_RANK/COMM_SIZE/COMM_HOSTS/COMM_PORT` (node rank, node count), opens the inter-node communicator (four TCP meshes, one per APU thread d, joining thread d of every node, rank = node; a whole-machine transform composes the node's xGMI exchange with the mesh exchange; `size` 1 = today's driver, bit for bit); a distributed-product self-test across all ranks at start; until M3, rank 0 computes and the others wait at the barrier; per-process META/RESULT lines, rank 0 the summary; `wp6run.sh` → `mnrun.sh <processes> [nodes]` | `size` 1 = baseline; 2 and 4 processes on one node; 2 on two nodes | 2 d |
| M2 | **Leaf partition by term range**: process r owns terms [N r/size, N (r+1)/size); seeds and the process-local levels exactly as today on its g APUs (the g regions with subtree ownership, as now); each process ends with P_r, Q_r as device numbers | P_r, Q_r vs the single-rank tree's node at that level (GMP-checked at 10⁶–10⁸; residues at 10¹⁰) | 2 d |
| M3 | **Distributed top levels**: level ℓ above the rank-local ones pairs rank groups of 2^ℓ; the pair's product through `rns_mul_dist` over the group's communicator (the four-APU tier with `size` = the group) with block-cyclic inside and the numbers contiguous per rank (each rank holds a contiguous 1/size of every number); the tree add and normalisation as today | `size` 2, 4: the final P, Q equal to the single-rank run's (identical digits) | 3 d |
| M4 | **Distributed division**: the reciprocal and the division on the whole-machine number (the dbig quarters become `size` shares), the window and corrections rank 0's | digits identical at `size` 1, 2, 4, 8 | 2 d |
| M5 | **Per-rank output and verification**: each rank formats its share of X (18-digit blocks) and writes its part file; T1 residues rank-local (P_r, Q_r by the recurrence over the rank's term range, X_r, R by Horner over the rank's limbs with the base power of the rank's offset) and combined by the communicator's reduction; T2 windows by the rank holding the position | `cat` of the part files identical to the single-rank output; T1/T2 pass at every size | 1 d |
| M6 | **Checkpoint per rank** at level boundaries (the WP7 format per rank, restart with the same `size`) | restart identical at `size` 4 | 1 d |
| M7 | **Hierarchical all-to-all** (xGMI group first, then the fabric) and slab pipelining (`_pre`/`_post`) | correctness only here; 8 ranks on two nodes | 2 d |
| M8 | **RDMA communicator** (libfabric or MPI behind `comm.h`) | on the target system | 2 d + tuning |
| M9 | Memory per rank at `size` 4: each rank's share of the planes (2 × 16 GiB stays), regions, block pool — the single-node profile divided by 4 per rank; the host share likewise | RSS per rank | in M2–M4 |

What exists already: `comm.h` with sim4/xGMI/TCP, `ntt_dist` (verified to
8 ranks across nodes), `rns_dist` on the four APUs, `dbig` quarters,
`wp6run.sh`. The phase-level overlap (§18) is done on the single-rank
driver first and carried into M1.

## 18. Phase 8 — overlap of disjoint work (started 2026-09-18)

The timeline (paper §7, Fig. 1): CPU periods 40 s, GPU periods 81 s, DMA
a few seconds, never overlapping. The easy version overlaps work that is
already independent, with threads, behind `ECALC_OVERLAP=1`:

| step | what runs concurrently | dependency kept | expected |
|---|---|---|---|
| O1 | init per APU in four threads (staging touch/register, plane pools, contexts) and the bs regions | peer access after all contexts exist | init 20 → ~10 s |
| O2 | T1's P, Q mod q by the term recurrence (CPU, 768 chunks) during the GPU levels of bs, on a bounded thread count | none (it uses N only) | T1 3.7 → ~0.5 s |
| O3 | during the reciprocal (GPU): P copied out, S = P + Q, A = 10ᵈ S on the CPU; Q not copied back (stays on device from the top level) | the division needs A: join before divmod | 10dP + copies 3.6 → hidden; Q in 0.6 → 0 |
| O4 | during the low product X Q and the corrections (GPU ≈ 9 s): X copied out first, then the digit formatting, the T2 windows and the digit residue on the CPU; if a correction changes X (0 of 6 runs so far) the formatting is redone | the final X | dc + T2 5.4 → hidden |
| O5 | the residues of X, R (Horner, CPU) overlapped with the frees and pool release | — | other 4 → ~2 |

Measured per step in RESULTS §68. Later (coordinated interleaving): I2, I3.

## 19. Phase 9 — work plan: all three tracks at once with parallel agents (proposed 2026-09-19, supersedes the three-track draft)

**Principle.** Agents run in parallel only where they own disjoint code;
where two items touch the same code they are folded into one agent. The
main session integrates: merges in a fixed order, re-verifies `main`
after every merge (10⁹ identical, size 1 bit-for-bit, one 2-process 10⁸),
schedules the nodes, writes the RESULTS sections. Each agent: its own
worktree and aac6 clone (`~/ntt-<name>`), its own job name, jobs ≤ 1 h
released between test batches, small-digit tests for multi-process work,
one full node only for 4 × 10¹⁰ timings; a `results/<name>.md` write-up;
never edits PLAN.md/RESULTS.md.

**Interfaces fixed up front** (so the agents can start together):
`mdb` (M3) is the sharded number; X, R, μ in the distributed division are
`mdb` over the full group; `comm_allgather(c, sendbuf, recvbuf, bytes)` is
added to `comm.h` on day 0 by the integrator (a naive point-to-point
implementation for every transport) so that every agent codes against it
and A-comm replaces the implementation later.

| agent | owns (files) | does | gate | days |
|---|---|---|---|---|
| **A-div** (M4 + B2 + B3 + C3) | `newton_db.c`, the driver's dm flow (`ecalc.c` between bs and the output) | the reciprocal and division as group products over the whole machine (S, Q, μ, X, R as `mdb`; window and corrections on shares with the cross-node carry scan; residues of R by the sharded kernel + reduction); the transform reuse across the last doubling and the division and the A·μ piece skip, written once in this tier; the reciprocal's pool policy at 6–7 × 10¹⁰ | sizes 1–4 on one node and 2–3 real nodes at 10⁸–10⁹ identical; size 1 bit-for-bit; 4 × 10¹⁰ size 1 timing ≤ baseline | 2.5 |
| **A-out** (M5 + C1) | `verify.c`, new `mn_out.c`, the driver's output tail | each node formats and writes its share of X as a part file, streamed in chunks (no 40 GB host string on any node, also at size 1); T1 residues rank-local (P_r, Q_r by the term recurrence over the node's range, X and R shares by the device kernel with the share's base power) combined by a mod-q reduction; T2 windows by the node holding the position. Developed against a stand-in sharding of X (node 0 scatters its X over the mesh) until A-div lands | part files concatenated identical to the reference at sizes 1–4; host peak at size 1: 70 → ≈ 30 GB | 1.5 |
| **A-grid** (A3 + C5) | `rns_dist.c` (the `_mn` product and the plane pools), `mdb.h` | the grid split over shares (share views with a global offset in the pack kernel, a shifted distributed add) for products beyond 2^(31+log₂ g_t) points; the 3·2³⁰ plane variant measured for the top levels | the first 4 × 10¹⁰ at size 2 on one node (memory shares) identical; `t_dbig big`-style checks over shares | 1.5 |
| **A-ckpt** (M6) | the checkpoint code in `binsplit.c`, tree-level save/restore in `mn.c` | per-node checkpoints at level boundaries including the tree levels' shares; restart with the same size | restart identical at sizes 2 and 4 at 10⁸–10⁹; single-node restart unchanged | 1 |
| **A-comm** (M7 + allgather) | `comm.h`, `comm_*.c`, `ntt_dist.c`, `comm_layered.c` | a real `allgather` in every transport; slab pipelining (local pass of slab k+1 under the exchange of slab k) through `dist_fwd_pre/_post` in the layered communicator; the O(g) loops replaced | `t_dist` layered at 2–4 node-processes and on real nodes; digits identical at sizes 2–4 | 1.5 |
| **A-mem** (M9 + C4 + C2) | `mem.c`, `rns_init`, `binsplit_pregrow` and the level layout in `binsplit.c` | memory per node accounted (planes, regions, block pool, packed slabs, layered scratch, host) and printed; init's pool sizing (planes at 3q, regions from one allocation per device); the region-0 imbalance at the five-node level | 4 × 10¹⁰ at size 1: init −1…−3 s, digits identical; the per-node capacity at size 2 and 4 measured | 1.5 |
| **N-kernel** (B1 + B4) | `ntt.c`, the batch-local path of `rns_mul.c` | Q₂ transformed once per pair in the batch tier (paired y-index in the fused inverse, power-of-two levels); the transform body's last LDS exchange via DPP/`ds_swizzle` | `t_ntt` bit-identical where the kernels are unchanged, GMP elsewhere; 4 × 10¹⁰ size 1 timing (−4…−6 s expected), identical digits | 2 |

**Integration order** (as each gate passes): A-grid → A-div → A-out →
A-comm → A-ckpt → A-mem → N-kernel (rebased last: it changes kernels
every other agent's tests exercise, so it is verified against the final
tree). After the last merge: the five-run variance at 4 × 10¹⁰ (size 1),
the multi-process 4 × 10¹⁰ at size 2, the ceiling at size 1, RESULTS §74+,
the paper's §7 and §10, the PLAN log. Then A7 (RDMA) on the target.

**Nodes.** aac6 has 3 usable nodes; multi-process tests share a node
(4 node-processes on one node), so up to three agents test at once and
timing runs (N-kernel, A-mem, A-div's baseline check) queue behind them
for their 2–5 minutes; agents are told to release allocations between
batches and to keep jobs ≤ 1 h.

**Elapsed:** ≈ 3 days (A-div's length) instead of ≈ 8 sequential; the
integrator's re-verification after each merge ≈ 20 min each.

## 20. The backlog after Phase 9 (2026-09-19) — every remaining item in one list

State: `main` @ 53730f8; 4 × 10¹⁰ in 86.4 ± 1.3 s / 48.8 GB host / ≈ 220 GB
device; 7 × 10¹⁰ in 163.8 s / 76.5 GB. The node has 502 GB; at 4 × 10¹⁰ about
230 GB are unused. Items grouped; sizes from the record; the user decides.

**A. Throughput, single node (GPU phases; the cross-resource overlap is spent)**
| # | item | expected | cost |
|---|---|---|---|
| A1 | I4 transform cache: fwd(Q)'s pieces kept across the reciprocal's last doubling and X·Q (48 GiB/APU of planes from the block pool — affordable now; the API change is written in results/A-div.md) | −3…−5 s of dm | 2 d |
| A2 | pair the level-22 products (4 pairs at 3·2²⁸: needs 51 GB planes — the 3·2³⁰ plane variant, now affordable) | −1.5 s | with B3 |
| A3 | I16 the reciprocal's block pool at 6–7 × 10¹⁰: `ECALC_DM_POOL=1` sizes it once from the scratch estimate (built, off; measured only at 4 × 10¹⁰ where it changes nothing) — measure at 7 × 10¹⁰ | −10…−15 s at 7 × 10¹⁰ | 0.5 d (node time) |
| A4 | the batch tier's tile budget in pair mode (15 GB; the pools allow 2²⁹ points per prime plane) | unknown, ≤ 1 s | 0.5 d |
| A5 | I9 over shares: the `lowcut`/`highcut` parameters of the grid over shares (A-grid's interface), so the distributed division skips the same pieces the single-node one does | multi-node dm −10 % | 0.5 d |
| A6 | the distributed tier's local transforms use the FULL pointwise and the tile kernel (N-kernel's pair mode and body 1 do not reach it) | −1…−2 s of the top levels/dm | 1 d |
| A7 | I8 decimal `mul_1` in the seeds (hidden now — only matters if init shrinks below the seeds' 8 s) | 0 today | — |

**B. Memory and resizing (230 GB of device memory idle at 4 × 10¹⁰; host 48.8 GB)**
| # | item | expected | cost |
|---|---|---|---|
| B1 | X never on the host at size 1: A-div's device X into A-out's writer (`src.dev`, the HOOK A-div lines in `out_stage`) — the 17.8 GB host X goes; the residues of P, Q, R per share instead of node 0's | host 48.8 → ≈ 31 GB; the two flows joined | 0.5 d |
| B2 | the pinned staging 43 GB: the seeds could stage per region in turn (one 10 GB buffer reused) | host −30 GB → ≈ 20 GB at size 1 | 1 d |
| B3 | 3·2³⁰-point planes for the top levels and level 22, sized at init (A-grid measured −2.5 s and a 15 s first-use cost that init sizing removes) | −2.5…−4 s for +136 GB device | 1 d |
| B4 | larger digit counts per node: with the host at 77 GB at 7 × 10¹⁰ the limit is device memory (regions ≈ 195 GB + planes 120 + the dm pool at 8 × 10¹⁰); the second parity's regions could be reused by the dm pool earlier and the planes shrunk after bs | 8 × 10¹⁰ on one node | 1 d |
| B5 | the region arenas of non-zero ranks released at their `rns_shutdown` (22 GB per process stays mapped until exit — harmless) | — | one line |
| B6 | at size > 1 the leaf's regions donated to the block pool before `mn_tree` also when the leaf ends on the batch tier (the tree's slabs come from `hipMalloc`, 11–44 GB per process) | multi-node −1…−3 s per node | 0.5 d |
| B7 | `mdb_shift` / `mdb_add_shifted` / the redistribution pad slabs to a round size (g × slab per APU): an all-to-all-v | memory −g × 2²⁶ × 8 B per APU per buffer; fine to hundreds of nodes, needed at 2 048 | 1 d |

**C. Scaling work (multi-node)**
| # | item | expected | cost |
|---|---|---|---|
| C1 | M8 the RDMA communicator behind `comm.h` (libfabric or MPI): all-to-all, all-gather, sub-communicators per level (replacing the per-level TCP meshes and their port slots) | the target system's numbers | 2 d + tuning, on the target |
| C2 | 4 × 10¹⁰ over two real nodes — tried 2026-09-20 (job 20760): both processes healthy at 263 GB each, but over aac6's 1 GbE a 2³¹-point distributed product costs ≈ 15–20 min (12 layered all-to-alls of ≈ 8.6 GB across the link at ≈ 110 MB/s plus the operand redistributions), the division has dozens → hours; cancelled after 30 min. The fabric, not the code (10⁹ at size 2 passes on two nodes). A target-system item; on aac6 the largest sensible two-node run is ≈ 5 × 10⁹ | verification at scale | on the target |
| C3 | load balance at non-power-of-two sizes (nodes beyond the largest power of two only redistribute) | only if the target size is not a power of two | 1 d |
| C4 | the forward slab pipeline cannot overlap the unpack of chunk k with the row pass of k+1 (both in x); a second plane for the column layout would | more of the exchange hidden | 1 d |
| C5 | `DIST_STATS` in chunked mode reports only the exposed exchange (the row pass and packs run under it) | reporting | 0.25 d |
| C6 | the multi-node checkpoints: tree sets written at every level with no `EVERY`; the barrier on the critical path; not run at 10¹⁰+ | robustness at scale | 0.5 d |
| C7 | the file write at scale: on aac6 the 40 GB write cannot hide (0.5–1.4 GB/s); on the target's parallel file system each node writes its part — measure there; a run without an output file skips the write | — | on the target |

**D. Tests and verification to add**
| # | item |
|---|---|
| D1 | a 5 × 10¹⁰ or 6 × 10¹⁰ run at size 2 over two real nodes with the part files compared against results/e_5e10.out |
| D2 | the restart at 10¹⁰ with tree-level sets at size 4 (only 10⁸–10⁹ so far) |
| D3 | `t_mn_grid` and `t_out` in `accept.sh`; `mnrun.sh` sizes 2/3/4 as a standing regression (the integrator's `verify*.sh` scripts, made permanent) |
| D4 | the binary path (`LIMB_BASE=2`) at 10¹⁰ after Phase 9 (10⁹ verified) |
| D5 | a stale-plane-pointer suspicion in the striped batch tier when a plane pool grows mid-phase (A-mem saw wrong P once; the growth is now never exercised at the defaults) — reproduce or rule out |

**E. Adjustments and housekeeping**
| # | item |
|---|---|
| E1 | remove the rejected variants' code paths or keep them as measurement evidence (`NTT_B16_XCHG`, `DIST_R3`, `ECALC_POOL_GROW_GB`, `BS_REGION_SLACK`): decide |
| E2 | `results/` is git-ignored; the agents' write-ups were force-added — decide whether `results/*.md` becomes tracked by rule |
| E3 | ecalc/README.md: the multi-node run (`mnrun.sh`, `COMM_*`, `MN_*`, `BS_CKPT_*` per node, `MEM_REPORT_DEVS`), the part files, the size-1 defaults after Phase 9 |
| E4 | the paper's §7 figure for a multi-node timeline once the target system exists |

## 21. Phase 10 — a 4–5 hour autonomous session on the §20 backlog (proposed 2026-09-19; executed 2026-09-20, RESULTS §75)

Same method as §19: agents own disjoint files, the integrator merges and
re-verifies, three nodes shared under the §19 protocol. The session is
sized so that every agent finishes inside it; the two-real-node runs are
the integrator's and depend on node availability.

| agent | owns | items (in order) | gate |
|---|---|---|---|
| **G** grid/dm | `rns_dist.c`, `mdb.h`, `newton_db.c`, the dm flow of `ecalc.c` | A5 the high/low cut parameters of the grid over shares and the sharded division using them; A1 the transform cache of Q's pieces across the last doubling and X·Q (single node first, `rns_mul_dist_db_cached`; the planes from the block pool); A6 the distributed tier's local transforms on the paired/register-blocked kernels where they apply | 10⁸/10⁹ at sizes 1–4 identical; single node 4 × 10¹⁰ identical and faster (A1 target −3 s); `t_mn_grid`, `t_dbig big` green |
| **H** host memory | `mn_out.c`, `verify.c`, the output tail of `ecalc.c`, the seeds' staging in `binsplit.c` and `rns_init` (coordinated with M) | B1 X never on the host at size 1 (A-div's device X into the writer; P, Q, R residues per share); B2 the seeds staged per region through one reused buffer | size 1 4 × 10¹⁰ identical with host peak ≈ 20–31 GB and wall unchanged; sizes 2–4 identical |
| **M** device memory | `mem.c`, pools in `rns_mul.c`, `binsplit_pregrow`/layout, the block pool's regions in `dbig.c` | B5 (one line); B6 the leaf's regions donated before the tree at size > 1; B4 the dm pool from the second parity's regions and the planes shrunk after bs; A3 measured at 7 × 10¹⁰ with `ECALC_DM_POOL=1`; then the 8 × 10¹⁰ attempt | 4 × 10¹⁰ identical; 7 × 10¹⁰ faster (A3); 8 × 10¹⁰ VERIFY OK or the exact memory that stops it |
| **C** communication | `comm*.c/.h`, `ntt_dist.c`, `mn.c` (groups/meshes), `tests/t_dist.c` | B7 an `alltoallv` op in every transport (the redistribution's padding — the use in `rns_dist.c` is G's, C provides the op and a t_dist check); C4 the second plane for the forward pipeline's overlap; C5 `DIST_STATS` under pipelining | t_dist all modes at 2–4 node-processes and 2 real nodes; xGMI 2³¹ timing not slower; 10⁸ sizes 2, 4 identical |
| **T** tests & docs | `accept.sh`, `variance.sh`, the integrator's `verify*.sh` → `mnaccept.sh`, `tests/`, `ecalc/README.md`, `binsplit.c`'s checkpoint code (C6) | D3 the standing regression (unit tests, 10⁹ both bases, sizes 2/3/4 at 10⁸, 10⁹ at 2/4, a restart, one 4 × 10¹⁰) as one script with a pass/fail summary; D2 restart at 10¹⁰ from tree-level sets at size 4; D4 the binary path at 10¹⁰; D5 the stale-plane-pointer suspicion reproduced or ruled out (read-only in N-kernel's tier: report, do not fix unless trivial); C6 tree-level `BS_CKPT_EVERY`, the barrier off the critical path; E3 README | the regression script green on `main`; D2/D4 identical; a written verdict on D5 |
| **integrator** | merges, `main` re-verified after each, RESULTS §75, PLAN log; C2 and D1 when two nodes are idle (4 × 10¹⁰ and 5 × 10¹⁰ at size 2 over two real nodes, parts compared with the references); E1/E2 proposed to the user | |

Timeline (hours): 0 — day-0 none needed (interfaces exist); launch all five. 0–3 — agents work; T's regression script is merged first as soon as it is green so the integrator's re-verification uses it. 2–4 — merges in readiness order (C → G → H → M → T), re-verification after each; C2/D1 whenever two nodes are idle (the integrator polls `sinfo`). 4–5 — closing measurements (five runs at 4 × 10¹⁰, the ceiling), RESULTS §75, paper §7/§10 numbers, PLAN log.

Expected outcome: single node ≈ 80–83 s / ≈ 25 GB host at 4 × 10¹⁰; 7 × 10¹⁰ ≈ 150 s; 8 × 10¹⁰ known (fits or the exact wall); the multi-node code without its padded exchanges and with the division's cuts; a standing regression; the checkpoints ready for scale; the first multi-process 4 × 10¹⁰ if two nodes come free. Not in this session: M8 (target system), C3 (only for non-power-of-two sizes), E4.

## 22. Deferred — the items not in the Phase 10 session (§21), kept for later (2026-09-19)

Everything below is recorded here (the single plan file) so nothing is
lost; each carries its origin, its sizing where one exists, and what it
waits for.

| # | item | origin | sizing | waits for |
|---|---|---|---|---|
| A2 | pair the level-22 products (4 pairs at 3·2²⁸: planes of 51 GB) | N-kernel open issue | −1.5 s | B3 (the larger planes) |
| A4 | the batch tier's tile budget in pair mode (15 GB; the pools allow 2²⁹ points per prime plane) | N-kernel | ≤ 1 s, unknown | node time |
| A7 / I8 | decimal `mul_1` in the seeds (two limbs per step or a Montgomery-style reduction) | §16 | 0 today (the seeds are hidden inside init) | init shrinking below ≈ 8 s |
| B3 / I10 | 3·2³⁰-point planes for the top levels, sized at init (A-grid: −2.5 s and a 15 s first-use cost) | §16, A-grid C5 | −2.5…−4 s for +136 GB device | a decision on the memory trade |
| C1 / M8 | the RDMA communicator (libfabric or MPI) behind `comm.h`; sub-communicators per level replacing the per-level TCP meshes and port slots | §17 | the target system's numbers | the target system |
| C3 | load balance at non-power-of-two sizes (nodes beyond the largest power of two in a group only redistribute) | M3 open issue | only if the target size is not a power of two | a decision on the target size |
| C7 | the part-file write at scale: on aac6 a 40 GB write cannot hide (0.5–1.4 GB/s); on a parallel file system each node writes its part under the low product — measure there | A-out open issue | — | the target system |
| E1 | the rejected variants' switches (`NTT_B16_XCHG`, `DIST_R3`, `ECALC_POOL_GROW_GB`, `BS_REGION_SLACK`, `ECALC_OVERLAP_COPY`): delete or keep as measurement evidence | Phase 8–9 | housekeeping | the user's decision |
| E2 | `results/` is git-ignored and the write-ups were force-added: track `results/*.md` by rule | Phase 9 | housekeeping | the user's decision |
| E4 | a multi-node timeline in the paper's §7 | paper | — | the target system |
| I5 | Karatsuba for products whose half-sums fit a plane (binary's 2 × 2 products only; decimal's do not fit) | §16 | binary −2.5 s | interest in the binary path |
| I11 | init: the transform contexts and tables built once and shared, pool zeroing by kernels | §16 | −1…−2 s of init (partly done by A-mem's sizing) | — |
| I13 | the two-prime 62-bit engine for the batch tier alone (rejected end to end in §44, never measured for the batch levels) | §16 | unknown, likely none | idle node time |
| — | `mdb_to_host_all` replicates a small number on every host (Q's top for the seed chain, the never-observed overshoot shrink): a broadcast would do | A-div open issue | tiny | — |
| — | the sim4 communicator stays unpipelined (`inflight` 0) | A-comm | tests only | — |
| — | B4's follow-on if 8 × 10¹⁰ does not fit: the regions' second parity reused by the dm pool is in §21; beyond that, planes at 2³⁰ for the dm phase with more pieces | §20 | 8 × 10¹⁰ on one node | the §21 result |
| — | the 3-node real run at 10⁹ and the 3-node checkpoint restart (only 2 real nodes were free together during Phase 9; 3 node-processes on one node cover the logic) | A-out, A-ckpt | verification | three idle nodes |

## 23. The decisions open after Phase 10 — options with costs and benefits (2026-09-20)

Every item is behind a switch or untouched; nothing changes until the
user decides. Sizes from RESULTS §74–§75 and results/{G,H,M,C,T}.md.

| # | decision | options | cost | benefit / risk |
|---|---|---|---|---|
| 1 (E1) | the rejected variants' code paths: `NTT_B16_XCHG` (DPP exchange), `DIST_R3`, `ECALC_POOL_GROW_GB`, `BS_REGION_SLACK`, `ECALC_OVERLAP_COPY`, `DIST_PLANE2` (`dist_fwd2/inv2`), `BS_SEED_DIRECT=0` (seed DMA per chunk), `COMM_PUSH64=0`/`COMM_PUSH_BLOCKS=228` (old push kernel), `RNS_DIST_CACHE` at size 1, the host-flow stand-ins `MN_DM=host`/`MN_COMBINE=host` (node 0's scatter/broadcast of X) | (a) delete all — ≈ 900 lines, 0.5 d plus a regression pass; (b) keep all as they are; (c) delete the ones with a maintenance cost and no measurement left to make (`DIST_PLANE2`, `BS_SEED_DIRECT=0`, the host-flow stand-ins, `ECALC_OVERLAP_COPY`), keep the one-line switches (`NTT_B16_XCHG`, `DIST_R3`, `COMM_PUSH*`, `RNS_DIST_CACHE`, `BS_REGION_SLACK`) | (a)/(c): 0.5 d each; every deleted path is one fewer branch every future agent must keep working (the host-flow stand-ins touch `out_stage` and `newton_mn_divmod`, the two files most edited); (b): nothing now, but each rejected path is re-tested at every merge for no reason and the size-1/size>1 flows keep two code shapes | (b) keeps the measurement evidence runnable (RESULTS cites the switches); (c) loses only re-runs no one plans — the numbers stay in RESULTS; (a) additionally removes `MN_DM=host`, the only cross-check of the sharded division against the host one at size > 1 (T's D5 failure is on that residue path — keep the stand-in until D5 is closed). Recommend (c) after decision 6. |
| 2 (E2) | `results/*.md` is git-ignored and force-added | (a) track `results/*.md` by rule (`!results/*.md` in .gitignore); (b) leave force-adding | (a) one line; (b) an agent that forgets `-f` loses its write-up | (a) — no downside |
| 3 (A3) | `ECALC_DM_POOL` default (the division's block pool pre-grown once, sized to hold t1's quarter) | (a) off (now); (b) on always; (c) on above a digit count (≈ 6 × 10¹⁰) | (b): at 4 × 10¹⁰ +0.6 s (85.9 vs 85.3, within noise) and the same bytes mapped; at 8 × 10¹⁰ the pre-grown chunk is 17.8 GB mapped at init instead of inside the reciprocal — same peak; (c): one comparison in `ecalc.c` | 7 × 10¹⁰: 159.7 → 157.2 s (−1.6 %); 8 × 10¹⁰ was run with it on. No correctness risk (VERIFY OK, same digits). Recommend (c) with the threshold at 5 × 10¹⁰. |
| 4 (B7) | the `alltoallv` consumers: `mdb_shift`, `mdb_add_shifted`, the redistribution in `rns_mul_dist_mn`, `mdb_to_host_all` → `comm_allgather` | (a) now, on aac6 (1–2 d; testable to size 4 on one node and 2–3 real nodes at ≤ 10⁹); (b) `mdb_shift` only now (0.5 d) — the one that grows with g; (c) on the target with M8 | (a): the sharded shifts and the redistribution are G's most intricate code; the change is mechanical but every path is re-verified through `t_mn_grid` + the regression; (b): smallest change, largest share of the gain; (c): no work now, but at 2 048 nodes `mdb_shift` allocates g × share/4 × 8 B per APU — 2 048 × 2²⁶ × 8 B = 1.1 TB: it cannot run without it | scratch at 4 × 10¹⁰ over 2 nodes: −17.6 GB per node (`mdb_shift`), −g × 512 MB per APU (`mdb_add_shifted`); the redistribution's padding is only 2 rows per pair (nothing to save, but the binary search in `k_gather_mn` goes). Required for 2 048; optional below ~100 nodes. Recommend (b) now, the rest with M8. |
| 5 (M open) | the arena laid out with t1's room as a contiguous tail | (a) do it (1 d: the simulated layout gains one more item, the block pool's first fit prefers the tail); (b) leave | (a) the dm phase maps 62–89 GB less (4–8 × 10¹⁰) — memory only, the wall is unchanged (mapping 0.06 s/GB is inside the reciprocal today: ≈ 4 s at 7 × 10¹⁰ — measured gain of the pre-grow was 1.7 s, so ≤ 2 s) | pushes the one-node ceiling from 8 × 10¹⁰ toward 10¹¹ (the accounting says the next line is near 10¹¹ with it); nothing at 4 × 10¹⁰. Recommend (b) unless > 8 × 10¹⁰ per node matters. |
| 6 (D5) | the intermittent size > 1 verification failure (1 of 18 forced-growth runs at 10¹⁰/4: six of eight primes BAD on P and Q, q2 and q6 fine — the per-prime signature of a residue-path fault, not a wrong number; A-mem saw the same once) | (a) investigate now: 1–2 d of node time — the residue kernel `db_mod_qs` over shares under pool growth, `mn_out.c`'s reduction over nodes, a race between H's mutex around `db_mod_qs` and the fetch streams; (b) leave it recorded | (a): node time, no new feature; (b): a run at scale may report BAD on a correct result (a false negative costs a rerun of the verification, not of the computation — the residues could be recomputed alone if that path were separable, which it is not today) | (a) is the only item with a correctness bearing; the digits were right in every failing case (the string cmp'd identical), so the fault is in the checker. Recommend (a) before any multi-node campaign; not needed for the single-node result. |
| 7 (B3) | 3·2³⁰-point planes for the top levels and level 22 (A-grid measured −2.5 s and a 15 s first-use cost that init sizing would remove; A2 needs it) | (a) do it (1 d + A2 0.5 d); (b) leave | +136 GB of device memory at init (4 × 10¹⁰: 262 → ≈ 400 GB — fits; 7–8 × 10¹⁰: does not fit — it would have to be a size-dependent default); +8 s of mapping at init unless the planes replace part of the region arenas | −2.5…−4 s (A2's pairing of level 22: −1.5 s more) at 4 × 10¹⁰: ≈ 5 % of the wall, the largest single-node item left. Recommend (a) as a switch on by default below 5 × 10¹⁰. |
| 8 (C2/D1) | 4 × 10¹⁰ (and 5 × 10¹⁰) over two real nodes | (a) on the target only; (b) a 5 × 10⁹ two-node run on aac6 now as the largest sensible cross-node check (0.5 h when two nodes are idle) | (a) nothing; (b) node availability (two idle nodes have not occurred together in two days) | (b) adds a real-fabric check at a size the 1 GbE allows; the code path is the same as 10⁹, already verified on two and three nodes. Recommend (b) opportunistically. |
| 9 | single-node throughput items left (§22): A4 tile budget in pair mode (≤ 1 s), I11 shared contexts/tables and pool zeroing by kernels (−1…−2 s of init), A7/I8 decimal `mul_1` (0 today), I5 Karatsuba (binary only, −2.5 s), I13 two-prime batch tier (unknown) | each 0.5–1 d; only A4 and I11 have a plausible gain on the decimal default | init at 16.4 s is 12 s of driver mapping — I11 attacks the other 4; A4 is a tuning knob | ≤ 3 s in total. Recommend after 7, if at all. |
| 10 (C3) | load balance at non-power-of-two node counts | (a) implement (1 d); (b) fix the target size at a power of two | (a) the group's redistribution gets a general block-cyclic map | depends on the target's node count (2 048 = 2¹¹ needs nothing). Recommend (b) unless the count is known to be otherwise. |
| 11 | what Phase 11 is | (a) single node: 7 + 3 + 9 (≈ −5…−8 s → ≈ 75–78 s); (b) multi-node hardening: 6 + 4 + 8, then M8 when the target exists; (c) both with two agents | (a) 2–3 d; (b) 2–4 d; (c) the §19 protocol | (a) moves the paper's number; (b) moves the 2 048-node readiness. Recommend (c): the tracks touch disjoint files (planes/init vs. mdb/comm/verify). |

## 24. Phase 11 — proposed: a multi-hour session with six agents on the three priorities (2026-09-20)

The user's priorities: (1) clean, efficient scaling to ~2 048 nodes, working
well at ~576, while staying fast and lean on one node; (2) speed from every
source — GPU compute, xGMI overlap, CPU overlap, less cross-node traffic,
interconnect hidden, general wall-clock; (3) minimum memory per digit.
576 = 9 · 2⁶ is not a power of two, which turns §23's decision 10 into a
requirement. aac6's compute nodes have OpenMPI, libfabric 1.20 and RCCL
(1 GbE between nodes), so an MPI transport can be built and verified now.

The proposal takes these positions on §23 (each still the user's call):
1 (c) after 6 · 2 (a) · 3 (c) · 4 (a, all consumers — required at 2 048) ·
5 (a — priority 3) · 6 (a) · 7 (a, on below 5 × 10¹⁰) · 8 (b) · 9 (A4, I11) ·
10 (a — 576) · 11 both tracks.

| agent | priority | owns | items (in order) | gate |
|---|---|---|---|---|
| **S** scaling transport | 1 | `comm_mpi.c` (new), `comm.h` (additive), `mn.c` (mesh/communicator creation), `mnrun.sh`, `tests/t_comm.c` | M8-a: an MPI transport behind `comm.h` — all-to-all, all-to-all-v, all-gather, barrier, max / sum-mod-q, point-to-point, `inflight` — with sub-communicators per tree level by `MPI_Comm_split` replacing the per-level TCP meshes and port slots; four APU threads per process on four communicators (`MPI_THREAD_MULTIPLE`) or one communicator with tagged slabs, measured both ways; `mnrun.sh` launching by `mpirun`; the TCP transport kept as the fallback | `t_comm`/`t_dist` in every mode at 2–8 processes on one node and 2–3 real nodes; 10⁸ at sizes 2, 3, 4 and 10⁹ at 2, 4 identical through MPI; the regression green with `COMM_TRANSPORT=mpi` |
| **L** layout at any size | 1 | `rns_dist.c`, `mdb.h`, `tests/t_mn_grid.c` | C3: a general block-cyclic map so a group of any size (3, 5, 6, 9, 18, 576) balances the distributed transform (nodes beyond the largest power of two no longer idle); B7: the four consumers moved to `alltoallv` (`mdb_shift`, `mdb_add_shifted`, the redistribution, `mdb_to_host_all` → allgather; C.md has the code) — the padded scratch (g × slab per APU) gone | `t_mn_grid` at 2, 3, 5, 6, 9 processes; 10⁸ at sizes 3, 5, 6 and 10⁹ at 3 identical; per-process scratch reported by `mem_report` at size 2 (−17.6 GB per node at 4 × 10¹⁰) |
| **X** cross-node traffic | 2 | `newton_db.c` (`recip_mn`, `newton_mn_divmod`), a new `mn_model.py`, `ntt_dist.c` stats only | X1: group size chosen per product size in the reciprocal's doublings and the division (the early doublings on 1, 4, 16… nodes rather than the whole group — latency-bound at 2 048) with the cost model deciding; X2: the fabric cost model — a script that takes the measured per-node numbers (this node's transforms, the xGMI exchange, the slab pipeline's hidden fraction) and the target fabric (400 GB/s per node, the layered all-to-all's message sizes) and gives per-node wall, bytes on the fabric, and exposed communication at g = 4, 64, 576, 2 048 for 4 and 8 × 10¹⁰ digits per node — the document that shows where the traffic is; X3: the transform cache's reach extended (Q's pieces held across more of the division's products where the block pool allows) | sizes 2–4 identical with X1 on; the division at size 4 not slower on one node; the model's numbers reproduced at g = 2, 4 on aac6 within 20 % (TCP scaled) |
| **P** single-node speed | 2 | `rns_mul.c` (pools), `binsplit.c` (level-22 pairing), the init of `ecalc.c`, `ntt.c` tile budget | B3: 3·2³⁰-point planes sized at init for the top levels (the 15 s first-use penalty gone), on by default below 5 × 10¹⁰; A2: the four level-22 products paired (shared operand transformed once); §23-3: `ECALC_DM_POOL` on above 5 × 10¹⁰; I11: contexts and twiddle tables built once and shared, pool clears by kernels; A4: the tile budget in pair mode | 10⁹ identical both bases; five runs at 4 × 10¹⁰ identical, target ≤ 79 s; 7 × 10¹⁰ not slower with the planes off |
| **V** verification and cleanup | 1, 2 | `verify.c`, `mn_out.c`, the residue kernel in `dbig.c`, `tests/`, then the rejected paths' files | D5 first: the per-share residues logged per prime before the cross-node reduction, the failure reproduced under forced growth at 10¹⁰/4 (T's recipe), the stage identified and fixed; then E1 (c): `DIST_PLANE2`, `BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY` and — once D5 is closed — the host-flow stand-ins deleted; E2: `!results/*.md`; a residue-only recheck mode (`ECALC_RECHECK=1`: recompute the Tier-1 residues from the digit file and the checkpointed P, Q without redoing the run) | 20 forced-growth runs at 10¹⁰/4 all VERIFY OK on every node; the regression green after the deletions; the recheck mode identical to the in-run check |
| **M** memory | 3 | `mem.c`, `dbig.c` (pool layout), `binsplit_pregrow`/the simulated layout, `mn_out.c` sizes only | §23-5: the arena laid out with t₁'s quarter as its contiguous tail (no mapping inside the division: −62…−89 GB); the tree's slabs at size > 1 sized into the arena (−5 GB per process at 10¹⁰/4); a memory model per node as a function of digits/g alongside X2 (device and host, every pool); then the next size on one node: 9 × 10¹⁰ or 10¹¹ attempted with the tail layout | 4 × 10¹⁰ identical with the pool's hipMalloc inside dm at 0; 8 × 10¹⁰ VERIFY OK at a lower peak; 10¹¹ VERIFY OK or the exact byte that stops it |
| **integrator** | | merges in readiness order, `mnaccept.sh --full` after each, the 5 × 10⁹ two-real-node run when two nodes are idle (§23-8), RESULTS §76, PLAN log, paper v4 numbers | | |

File ownership is disjoint except two additive seams: X's `recip_mn` group
choice needs `mn_group_at` for subgroups (S's `mn.c`, additive; agreed on
day 0), and P's `ecalc.c` init lines against V's deletions in the output
tail (different regions of the file). Node protocol as §19/§21: jobs
≤ 45 min named per agent, ≤ 3 queued, release after each batch; two idle
nodes plus the shared one.

Timeline (hours): 0 — launch all six. 0–4 — agents; V's D5 fix and L's
`t_mn_grid` at non-power-of-two sizes are the first merges (they unblock
E1 and X1). 3–6 — merges, regression after each, S's MPI regression run
against `main`. 6–8 — closing: five runs at 4 × 10¹⁰ (target ≤ 79 s),
8 × 10¹⁰ and the attempt beyond, the model's tables at 576 and 2 048
(X2/M) in RESULTS §76, paper v4.

Expected outcome: single node ≈ 78–80 s and ≈ 200 GB device at 4 × 10¹⁰;
one node beyond 8 × 10¹⁰; the multi-node code on MPI with sub-communicators
per level, balanced at any group size, without padded exchanges, with the
early doublings on small groups; a per-node model of time, traffic and
memory at 576 and 2 048 nodes; the verification path's intermittent fault
closed and a recheck mode; the rejected paths gone.

## 25. The target system (stated by the user 2026-09-20) — the design target for every decision from here on

| item | value | what it implies for the design |
|---|---|---|
| nodes | **576 MI300A nodes, 4 APUs each = 2 304 APUs**; the algorithm is optimised for this size, not for a generic 2 048 | 576 = 9 · 2⁶: the group sizes of the distributed tree levels are 2, 4, …, 64 and then a 9-way (or 3 · 3) step — the block-cyclic map must be general (§23-10 is a requirement, not an option); term ranges partition over any count already |
| fabric | **HPE Slingshot-2 in a Dragonfly topology, diameter 3** (three switch hops at most: local–global–local); Rosetta-class 64-port switches, groups all-to-all inside, groups all-to-all globally | locality has two tiers above xGMI: inside a dragonfly group (one switch hop) and across groups (a global link, the scarce resource). The tree's node groups should be placed so that the low distributed levels stay inside a dragonfly group and only the top levels and the division cross global links; the layered all-to-all gets a third layer (APU × node-in-group × group) so that inter-group traffic is aggregated per peer group. The group size (nodes per dragonfly group) is a run parameter (`MN_TOPO_GROUP`) until the machine's cabling is known |
| NICs | **two 400 Gb/s NICs per APU, eight per node**: 100 GB/s per APU, 400 GB/s injection per node | one APU thread drives its own two NICs: the four-communicator-per-node structure (mesh d = APU thread d) maps directly onto one SHMEM context per APU thread; per-APU fabric bandwidth (100 GB/s) is about one xGMI link (91 GB/s), so a distributed product over the fabric costs roughly what the same product costs over one xGMI link per APU — the slab pipeline's overlap matters as much on the fabric as on xGMI |
| programming model | **SHMEM, not MPI** — Cray OpenSHMEMX expected; rocSHMEM or OpenSHMEM acceptable | one-sided puts into a symmetric heap = the push model the xGMI transport already uses (each APU pushes its slabs into the peers' receive buffers, completion by a signal). The transport is written against the OpenSHMEM 1.4/1.5 subset the three share: `shmem_malloc` symmetric heap (device memory on Cray/rocSHMEM; host memory registered with HIP on aac6's OSHMEM), `shmem_ctx_create` (one context per APU thread), `shmem_putmem_nbi` / `shmem_put_signal` (or put + fence + flag where absent), `shmem_quiet`, `shmem_barrier_all`, `shmem_team_split_strided` (OpenSHMEM 1.5; teams = the tree's node groups), reductions. rocSHMEM's GPU-initiated puts are a later option for kernel-side slab posting, not required |
| what aac6 offers now | OpenMPI 4.1.6 with **OSHMEM** (`oshcc`, `oshrun`, OpenSHMEM 1.4), libfabric 1.20, RCCL; no rocSHMEM; 1 GbE between nodes | the SHMEM transport can be built and verified on aac6 now (correctness at 2–3 real nodes, all sizes on one node through the shared-memory transport); its bandwidth numbers come from the target only. Teams (1.5) are absent in OSHMEM 4.1: the transport carries a small compatibility layer (a team = a strided PE set, sub-communicators built on it) |

Sizing at the target (from the measured cell; refined by X2's model): per node 4–8 × 10¹⁰ digits at the single-node profile (83 s / 11.7 GB host / ≈ 200–260 GB device), i.e. 2.3–4.6 × 10¹³ digits over 576 nodes; the distributed levels above the node: 6 power-of-two levels inside dragonfly groups (if a group holds ≥ 64 nodes) and the 9-way top level plus the division across groups.

## 26. Phase 11 (revised for §25) — a multi-hour session with six agents (proposed 2026-09-20; executed 2026-09-20/21, RESULTS §76; supersedes §24)

The §24 plan with the transport agent redirected from MPI to SHMEM, the layout agent sized to 576, the traffic agent given the dragonfly's two tiers, and the models parameterised by the target. Positions on §23 unchanged (1 (c) after 6 · 2 (a) · 3 (c) · 4 (a) · 5 (a) · 6 (a) · 7 (a) · 8 (b) · 9 (A4, I11) · 10 (a, required) · 11 both).

| agent | priority | owns | items (in order) | gate |
|---|---|---|---|---|
| **S** SHMEM transport | 1 | `comm_shmem.c` (new), `comm.h` (additive), `mn.c` (communicator creation), `mnrun.sh`, `Makefile` (`oshcc`), `tests/t_comm.c` | M8-s: the transport behind `comm.h` on the OpenSHMEM subset of §25 — the symmetric heap holding the receive slabs (device memory where the implementation allows, HIP-registered host memory on aac6), one context per APU thread, push by `putmem_nbi` + signal, `wait` by the signals, the all-to-all-v by counts exchanged first, all-gather, barrier, max / sum-mod-q by the reductions, point-to-point small values; sub-communicators per tree level as strided PE sets (teams where the implementation has them, a shim otherwise), replacing the per-level TCP meshes and port slots; `mnrun.sh` launching by `oshrun` (`srun` on the target); the TCP transport kept as the fallback. Then the third layer of the layered all-to-all (§25: group-in-dragonfly), behind `MN_TOPO_GROUP` | `t_comm`/`t_dist` in every mode at 2–8 PEs on one node and 2–3 real nodes; 10⁸ at sizes 2, 3, 4 and 10⁹ at 2, 4 identical through SHMEM; the regression green with `COMM_TRANSPORT=shmem`; the three-layer all-to-all identical at 8 PEs with `MN_TOPO_GROUP=4` |
| **L** layout at 576 | 1 | `rns_dist.c`, `mdb.h`, `tests/t_mn_grid.c` | C3-576: a general block-cyclic map for any group size, with the 9-way (and 3-way) steps the 576-node tree needs, the level→group-size schedule chosen by cost (2, 4, …, 64, 576 or …, 64, 192, 576) and settable (`MN_GROUPS=2,4,8,16,32,64,576`); B7: the four consumers on `alltoallv` (`mdb_shift`, `mdb_add_shifted`, the redistribution, `mdb_to_host_all` → allgather); the padded scratch gone | `t_mn_grid` at 2, 3, 5, 6, 9 processes; 10⁸ at sizes 3, 6, 9 and 10⁹ at 3 identical (nine processes at 10⁸ fit one node); the scratch per process at size 2 down 17.6 GB per node at 4 × 10¹⁰ |
| **X** cross-fabric traffic | 2 | `newton_db.c` (`recip_mn`, `newton_mn_divmod`), `mn_model.py` (new), `ntt_dist.c` stats only | X1: group size per product size in the reciprocal's doublings and the division (the early doublings on 1, 4, 16 … nodes inside one dragonfly group; the whole 576 only where the operand needs it), chosen by the model; X2: the fabric model for §25 — inputs: the measured per-node numbers (transform passes, the xGMI exchange, the slab pipeline's hidden fraction), 100 GB/s per APU, the dragonfly's two tiers (in-group vs global, `MN_TOPO_GROUP`), the layered all-to-all's message sizes; outputs per node: wall, bytes per NIC, global-link bytes, exposed communication, for 4 and 8 × 10¹⁰ digits per node at g = 4, 64, 576 — the document that says where the traffic is and what the third layer buys; X3: the transform cache's reach across more of the division's products | sizes 2–4 identical with X1 on; the division at size 4 not slower on one node; the model reproduces g = 2, 4 on aac6 within 20 % (TCP scaled) |
| **P** single-node speed | 2 | `rns_mul.c` (pools), `binsplit.c` (level-22 pairing), the init of `ecalc.c`, `ntt.c` tile budget | B3: 3·2³⁰-point planes sized at init for the top levels, on by default below 5 × 10¹⁰; A2: the level-22 products paired; §23-3: `ECALC_DM_POOL` on above 5 × 10¹⁰; I11: contexts and twiddle tables built once and shared, pool clears by kernels; A4: the tile budget in pair mode | 10⁹ identical both bases; five runs at 4 × 10¹⁰ identical, target ≤ 79 s; 7 × 10¹⁰ not slower with the planes off |
| **V** verification and cleanup | 1, 2 | `verify.c`, `mn_out.c`, the residue kernel in `dbig.c`, `tests/`, then the rejected paths' files | D5 first (per-share residues logged per prime before the cross-node reduction; the failure reproduced under forced growth at 10¹⁰/4 by T's recipe; the stage identified and fixed); then E1 (c) — `DIST_PLANE2`, `BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY`, and once D5 is closed the host-flow stand-ins — deleted; E2 `!results/*.md`; `ECALC_RECHECK=1`: the Tier-1 residues recomputed from the digit file and the checkpointed P, Q without redoing the run | 20 forced-growth runs at 10¹⁰/4 all VERIFY OK on every node; the regression green after the deletions; the recheck identical to the in-run check |
| **M** memory | 3 | `mem.c`, `dbig.c` (pool layout), `binsplit_pregrow`/the simulated layout, `mn_out.c` sizes only | §23-5: the arena with t₁'s quarter as its contiguous tail (no mapping inside the division: −62…−89 GB); the tree's slabs at size > 1 sized into the arena (−5 GB per process); the per-node memory model (device and host, every pool) as a function of digits per node and of g, joined to X2's tables for §25; then 10¹¹ on one node attempted with the tail layout | 4 × 10¹⁰ identical with zero hipMalloc inside dm; 8 × 10¹⁰ VERIFY OK at a lower peak; 10¹¹ VERIFY OK or the exact byte that stops it |
| **integrator** | | merges in readiness order (V's D5 and L's grid first — they unblock E1 and X1), `mnaccept.sh --full` after each and once more with `COMM_TRANSPORT=shmem`; the 5 × 10⁹ two-real-node run over SHMEM when two nodes are idle; RESULTS §76; PLAN log; paper v4 (the §25 target in §10) | | |

Seams (agreed on day 0, additive): X needs subgroup creation from S's `mn.c` (`mn_group_at(level, size)` exists; S keeps its signature); L's `MN_GROUPS` schedule is read by S's communicator creation (L defines the parser in `rns_dist.c`, S calls it); P's init lines and V's deletions are in different regions of `ecalc.c`. Node protocol as §21. Standing rule (user, 2026-09-20): every session's closing report and its RESULTS section state the **estimated maximum digits for a 576-node job and the estimated wall clock in minutes** for it, from the current per-node memory profile and the fabric model (X2/M); the estimate is labelled measured / modelled / extrapolated. Closing Phase 10 (before this rule): 576 × 8 × 10¹⁰ = 4.6 × 10¹³ digits on the verified per-node profile (≈ 5.8 × 10¹³ at the estimated 10¹¹-per-node ceiling), ≈ 5–6 min of wall plus the fabric time not yet modelled.

Timeline (hours): 0 launch; 0–4 agents (S's transport passes `t_comm` at hour ~2 and `t_dist` at ~3; L's grid at 9 processes by hour 2); 3–6 merges with the regression after each; 6–8 closing: five runs at 4 × 10¹⁰, 8 × 10¹⁰ and beyond, the §25 tables at g = 4 / 64 / 576 from X2 + M, paper v4.

Expected outcome: the multi-node code on SHMEM with per-level PE sets, balanced at 576, without padded exchanges, with the small doublings on small groups and a dragonfly-aware all-to-all; a per-node model of time, fabric bytes (per NIC and global) and memory at 576 nodes; one node ≈ 78–80 s and ≈ 200 GB at 4 × 10¹⁰ and beyond 8 × 10¹⁰; the checker's fault closed and a recheck mode; the rejected paths gone.

## 27. Phase 12 — the complete solutions to the DECISIONS2 items, six agents (proposed and executed 2026-09-21/22; RESULTS §77)

The brief: the best long-term solution to each open item, not the cheapest.
Each row names the complete form and how it is verified on aac6.

| agent | item(s) | the complete solution | owns | gate |
|---|---|---|---|---|
| **R** the race | DECISIONS2 #1 | Root cause, not avoidance: V's reproducer (`v11_d5.sh`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP`) run with the per-level probe moved one step at a time until the faulting transition is named; every CPU→GPU hand-over at a level boundary (`spill_merge`, the scatter, the CRT tree add, the plane-pool growth's copy) audited for stream ordering (events recorded on the producing stream, waited on the consuming one, no null-stream assumptions); the fix; then the invariant: a pool that would grow inside a phase aborts with `mem_oom`'s accounting unless `RNS_POOL_GROW=1` (the stress recipe sets it); a **stress step in `mnaccept.sh`** (`--stress`: 10 forced-growth runs at 10⁹/4, must be 10/10) so the race cannot return unnoticed | `rns_mul.c` (batch tier, pools' growth path), the level loop of `binsplit.c`, `mnaccept.sh` (the stress step only) | 40 forced-growth runs at 10¹⁰/4 without any probe: 40/40 identical; the stress step 10/10 in three separate batches; the regression 17/17 |
| **G** the top product at scale | #2 (+ the spill buffers) | The tree's top levels formed as **piece grids over fixed 2³¹-point planes for any g** (the single-node reciprocal's form and A-grid's grid over shares, applied to `mn_tree`'s products): pieces of the sharded operands, the shifted distributed add with the cross-node carry, the transform cache over pieces; the spill buffers (2 g C × 4 limbs per APU today) replaced by the gridded pieces' exact spills through `alltoallv`; the per-node memory profile independent of g (the memory model's g-terms become O(share)). Forced-grid tests (`DIST_LOGN_TEST`) at sizes 2–9 make the path run at 10⁸–10¹⁰ on one node | `rns_dist.c` (with L's map), `mn.c` (`mn_tree`), `mdb.h`, `tests/t_mn_grid.c` | `t_mn_grid` with forced grids at 2, 3, 4, 6, 9; 10⁸ at sizes 3, 6, 9 and 10⁹ at 2, 3, 4 with forced grids identical; 10¹⁰ at size 4 identical and the tree's device peak per process reported; the memory model's g-terms updated and matching |
| **I** the initialisation floor | #3 and the 12 s of mapping behind it | The mapping floor attacked at its source, then the planes decided on the result: (1) measure every allocation form on the APU for 200 GB — `hipMalloc`, `hipMallocAsync` from a `hipMemPool` with a release threshold, `hipMallocManaged`, `hipExtMallocWithFlags` (uncached / fine-grained), `hipHostMalloc` coherent (unified memory: the same HBM), `mmap` + THP + `hipHostRegister` — time to map, first-touch cost, kernel bandwidth from each (the transform kernel's TB/s must not drop); (2) the fastest form that keeps the kernel rate becomes the pool allocator; (3) the seeds moved out of the mapping window (mapping first, then the seed thread — or the reverse — measured both ways, the wall decides); (4) the 3·2³⁰ planes re-measured on the new floor and the default set by the number; (5) `ECALC_DM_POOL` deleted (a no-op with the tail) | `mem.c`, the pools' creation in `rns_mul.c`, the init of `ecalc.c`, `binsplit_pregrow` (with M's tail intact) | 10⁹ identical both bases; five runs at 4 × 10¹⁰ identical, init and wall reported per form; 8 × 10¹⁰ VERIFY OK on the new allocator; the kernel rates unchanged (`t_ntt` timings) |
| **S** the SHMEM transport's target forms | #4, #5, #9 | The forms the target uses, **tested for real on aac6** by building Sandia OpenSHMEM (SOS, user-space, libfabric `tcp`/`sockets` provider, `SHMEM_THREAD_MULTIPLE` and contexts supported) in `~/sos`: contexts per APU thread without the global lock (`COMM_SHMEM_SERIAL=0`), the symmetric heap of device memory (`COMM_SHMEM_DEVHEAP=1`; on the APU host-registered and device heaps are the same HBM — both paths kept, the target's Cray SHMEM decides), the callers' slabs resident in the pool (`comm_sym_alloc`: `ntt_dist`'s slab buffers allocated from the symmetric pool, no staging copy, no helper thread), `put_signal` under `#if` for 1.5 implementations; the third layer kept behind its switch with a `t_dist` timing at 8 PEs on both transports; the two-real-node SHMEM run at 10⁹ when two nodes are idle | `comm_shmem.c`, `comm.h`, `Makefile`, `mnrun.sh`, `ntt_dist.c` (slab allocation only), `tests/t_comm.c` | `t_comm`/`t_dist` every mode at 2–8 PEs on OSHMEM (serial) and on SOS (thread-multiple, device heap, pool-resident slabs); the regression over SOS 17/17; 2 real nodes at 10⁹ identical if available |
| **Q** the target plan | #6, #7, the standing estimate | The models completed for the target and turned into a runbook: L's real schedule in `mn_model.py` (9-way vs 3·3 decided by the model and left as `MN_GROUPS`); the memory model with G's gridded profile (per node independent of g) and the safe/ceiling sizes per node with margins; the per-run estimate the user asked for, as a function `estimate(g, D)` printing digits, minutes, GB per node, GB on the fabric; **`docs/TARGET.md`** — the run recipe on the target (environment, `srun` line, `MN_GROUPS`, `MN_TOPO_GROUP`, the sizes to run in order: safe, then the ceiling; checkpoints; the recheck; what to measure first to calibrate the model's assumptions — per-message cost, file bandwidth) | `mn_model.py`, `mem_model.py`, `docs/TARGET.md` (new) | the model reproduces every measured aac6 point within 10 %; the runbook reviewed against the code's switches (every named variable exists) |
| **W** verification and housekeeping | #8 | `ECALC_CKPT_TOP` on by default above 10¹⁰ (the recheck possible for every large run); the recheck (`ECALC_RECHECK=1`) as a regression step at sizes 1 and 2; the host-flow stand-ins deleted once R reports the race closed (else left, with the reason); README and the switch list brought to the final state; `results/*.md` verified tracked | `verify.c`, `mn_out.c`, the output tail of `ecalc.c`, `ecalc/README.md`, `mnaccept.sh` (the recheck step), `.gitignore` | the regression with the recheck step green; a 4 × 10¹⁰ run rechecked from its files |
| **integrator** | | merges (R and G first), the regression after each and over SOS, RESULTS §77, PLAN log, paper v5, the standing 576-node estimate from Q's function | | |

Seams: G's gridded tree and S's pool-resident slabs both touch `ntt_dist.c`
(S: allocation only; G: none — G works in `rns_dist.c`); I's pool creation
and R's growth path are both in `rns_mul.c` (I: creation; R: growth — agreed
on day 0); W's stand-in deletion waits for R's verdict (W does everything
else first). Timeline (hours): 0 launch; 0–5 agents; 3–7 merges with the
regression; 7–9 closing: five runs at 4 × 10¹⁰, 8 × 10¹⁰ and 10¹¹ on the new
allocator, the SOS regression, Q's estimate, RESULTS §77, paper v5.

Expected outcome: the race closed and guarded; the per-node profile
independent of g — the 576-node limit the single-node ceiling × 576
(≈ 4.4 × 10¹³ digits, ≈ 4.6 min); the initialisation floor reduced (up to
−10 s of the 81.5 if a mapping form keeps the kernel rate) and the larger
planes decided on that; the SHMEM transport in its target form verified
on a thread-multiple implementation; a runbook and a model that give the
digits, minutes and GB for any (g, D).

## 28. Code reduction — scheduled after all other work items (2026-09-22)

Recommendations and per-item reasoning: `CODE_REDUCTION.md`. The user has approved
groups (a), (b) and (e); group (c) (merging) and group (d) (retention) are not
scheduled here. **This work comes after every other item in §23, §26 and the
DECISIONS3 list**: it touches live code and its only justification is clarity, so it
must not compete with correctness or performance work for node time.

| step | content | core lines | risk |
|---|---|---|---|
| 28.1 | **(a) Archive the instruments** → `archive/instruments/`: `ECALC_RES_LOG`(+`_LEVEL`,`_CPU`), `ECALC_LEAF_DUMP`, `ECALC_COPY_PROBE`, `ECALC_B_SNAPSHOT`, `MEM_DPOOL_FILL`, `MEM_COPY_NOWAIT`, `DBIG_SERIAL`, `DBIG_WARM`, `MEM_NO_DEV_MEMSET`, the five trace switches. `tests/t_alloc.c` and `tests/t_copy_order.c` stay in `tests/` | −320 | none |
| 28.2 | **(b1) Remove engine 2**: `ntt2.c` (509), `ntt2.h` (66), `modarith2.h` (65), `crt2.c` (57), the 5 dispatch sites in `rns_mul.c`. Archived under `archive/engine2/`; measurement stays in RESULTS §44 | −757 | low (switch-only path) |
| 28.3 | **(b2) Remove the rest**: `newton.c` (231, → `archive/newton_host/`, `t_newton` re-pointed at `newton_db`), the host-flow stand-ins (~40), the rejected layout/placement switches (~200), `MEM_ALLOC`'s five losing forms (~70), the DPP body (~55), the legacy flow guards (~50). Keep `ECALC_OVERLAP=0` if scheduling work is foreseen | −646 | low |
| 28.4 | **(e0) Flip the library default to decimal** (`bi_decimal = 1`) and run the full regression plus the five base-2 tests in decimal, changing nothing else. This is the measurement that decides whether the binary pipeline still has anything to say. If a test fails or an oracle disagrees, stop and report — that is the cross-check earning its keep | 0 | none (measurement) |
| 28.5 | **(e) Archive the binary pipeline** once 28.4 is clean: `todec.c` + `todec.h` (344), the bit operations `limb_shr_bits` / `limb_shl_bits` and the bit paths of `bi_shl`/`bi_shr` (~57), ~45 `bi_decimal` branch sites across eleven files; `tests/t_dec.c` (86) removed; **`t_dbig`, `t_mul`, `t_crt`, `t_newton`, `t_mn_grid` rewritten to run in decimal**, each re-validated against GMP individually. Archive `todec.c` and one base-2 test (`t_mul`) so the comparison can be reconstructed. The regression loses its two-base step; §9 of the paper loses the independent-pipeline claim and must be amended | −446 core, −86 test | **medium**: five test programs rewritten, and a verification layer retired deliberately |
| 28.6 | `ecalc/README.md` switch list regenerated (it is grep-verified, so a stale entry fails loudly); `archive/MANIFEST.md` written; both papers' LOC figures and the paper's §9 updated | — | none |

**Totals**: core 13 016 → **≈ 10 850 (−2 166, 17 %)**; switches 116 → ≈ 55; tests
3 052 → ≈ 2 966 with five programs rewritten. Performance unchanged by construction —
every removed path is either unreachable at the defaults or measured worse.

**Gate for every step**: `mnaccept.sh <job> --full --stress` green before and after, and
a five-run $4\times10^{10}$ series after 28.3 and after 28.5 to confirm the wall clock is
untouched.

## 29. Phase 13 — the design-space campaign: measure every option, adopt the best (proposed 2026-09-22)

The open design questions (TASKS §6, and the K4 analysis that followed) are no longer
answerable by reasoning: the three distribution strategies, the prime count, the plane
cap and the transform-length set interact, and several of the hardware features have
never been measured on the paths that matter. This section is the campaign that settles
them by measurement.

### 29.0 The decision rule, fixed before any measurement

The target is 576 nodes and its capacity is set by **per-node memory**, so runtime alone
is the wrong objective. The figure of merit is

> **digits per node-second, subject to fitting the node**, with the Pareto frontier of
> (wall clock at 4 × 10¹⁰, peak node memory) reported for every configuration.

A configuration wins if it is Pareto-dominant. Where two are incomparable (faster but
larger), the tie is broken by **the machine-scale digit ceiling** from `estimate.py` at
576 nodes — that is the number the project exists to maximise. Every adoption is the
user's on the measured data, as always.

### 29.1 The parameter space

| axis | values to measure |
|---|---|
| primes $P$ | 3, 4 |
| distribution of one product | product-per-APU (A), prime-per-APU (B), four-step (C) |
| plane cap | $2^{29}$, $2^{30}$, $3\cdot2^{30}$, $2^{31}$ |
| transform lengths | $\{2^k, 3\cdot2^k\}$, $+\,5\cdot2^k$, $+\,7\cdot2^k$ |
| reciprocal products | full (today), middle + short product |
| seeds | CPU (today), GPU kernel |

### 29.2 Micro-benchmarks first — the decisive, cheap experiments

**E0 `tests/t_strategy` — the A/B/C boundary.** One product of $n$ points over four APUs,
timed under all three strategies at $n \in \{2^{26} \dots 2^{31}\}$, for $P = 3$ and 4,
reporting wall, peak plane bytes per APU, and exchange count. Prime-per-APU is built here
as a harness first (it need not be production code to be measured). **This single
benchmark settles where regime B begins and ends, and whether the grid's cap should be
lowered to make every piece exchange-free.** Minutes of node time; run before any
pipeline change.

**E1 `tests/t_primes` — the three-prime bound in practice.** CRT reconstruction and a
full convolution at $P = 3$ against GMP at every length from $2^{20}$ to $2^{33}$,
including the worst case (all limbs $B-1$), with the margin $\prod p_i / (nB^2)$ printed.
Confirms the arithmetic before any pipeline work: the margin is 27x at $2^{31}$ and 7x at
$2^{33}$, and a test must show that it is not being eaten by something unmodelled.

**E2 `tests/t_cap` — plane cap against grid cost.** For the product shapes the pipeline
actually forms (the top two tree levels, the reciprocal's last three doublings, the
division's two products at $4\times10^{10}$ and $10^{11}$), the total transform points and
the exchange count under each cap. Pure arithmetic over `split_grid_cap`, no node time;
tells us what E0's answer would cost in extra points before we measure it.

### 29.3 Pipeline experiments

**E3 — three primes end to end.** `EC_NP` parameterised, a 3-prime CRT, the `mdev` tier's
prime-per-device assignment generalised. Gate: $10^9$ digits byte-identical in both bases,
regression 21/21, then a five-run $4\times10^{10}$ series and a $10^{11}$ run with peak
memory. Expected: −25 % of transform work in regimes A and C, planes 180.4 → 135.3 GB.

**E4 — regime B in the reciprocal.** A device-resident prime-per-APU path for the
doublings whose product fits the per-APU budget, chosen by the boundary E0 measures.
Gate: reciprocal time and exchange count at $4\times10^{10}$ and $10^{11}$; digits identical.

**E5 — the exchange-free grid.** Cap lowered so every grid piece fits prime-per-APU,
making the distributed tier exchange-free at the cost of more pieces and more padding.
Gate: the top levels, reciprocal and division timed under both caps, with the total
transform points reported so the trade is visible. **This is the experiment that decides
whether the K4 structure can remove the all-to-all from a node entirely.**

**E6 — transform lengths $5\cdot2^k$ and $7\cdot2^k$.** Radix-5 and radix-7 stages on the
model of `ntt3.c`. Gate: `t_ntt` exact at the new lengths, then the batch tier's time.

**E7 — middle and short products in the reciprocal.** Gate: `t_newton` exact, reciprocal
time at $4\times10^{10}$ and $10^{11}$.

**E8 — the seeds as a kernel.** Gate: seed spans identical to the CPU version, init time,
and the wall.

### 29.4 Hardware experiments — exploit what the machine offers

**H1 CPX vs SPX.** The batch tier's products are independent and subtree-owned; CPX
exposes each XCD as its own partition. The mode is set at boot, so this is a request to
the cluster's administrators plus one measurement if a CPX node can be had. Measure: the
batch tier, the distributed tier, and `t_ntt`.

**H2 The MALL (256 MB last-level cache).** Sweep a transform kernel's working set from
below to well above 256 MB and find the cliff; then check whether a pass's tile column can
be kept resident. The kernels reach 1.0–1.4 TB/s against a 3.0 TB/s copy ceiling and were
tuned against LDS and HBM, never against the MALL.

**H3 The modmul engine on the paths that matter.** FP64 Barrett (today) against Shoup
integer butterflies (y-cruncher's choice) and against a reduced-correction FP64 variant,
measured on the *full* transform rather than the first pass, which is the only place it
was ever compared.

**H4 xGMI concurrency.** Whether the push saturates all three links simultaneously or
serialises: aggregate against per-link bandwidth, and whether a different peer ordering or
more concurrent streams helps.

**H5 SDMA engines.** Whether the $X$ fetches and the checkpoint writes can be moved off
the compute units onto the copy engines, freeing CUs during the division.

**H6 Occupancy and launch configuration** for the transform kernels at the sizes the
pipeline actually uses, revisited under whatever H2 finds.

**H7 Non-temporal stores in pack/unpack.** Those kernels are pure streaming and pollute
the cache the transform wants; a cache-bypassing store may help both.

**E9 xGMI and the fabric at the same time.** The layered all-to-all runs its intra-node
(xGMI) stage and its inter-node (fabric) stage as two phases. They use *disjoint*
hardware, so in principle the exchange costs $\max(T_\mathrm{intra}, T_\mathrm{inter})$
rather than their sum. Per APU the target offers 273 GB/s of xGMI (three links) against
100 GB/s of NIC injection (two 400 Gb/s ports), so with the layered form's traffic
($\approx S$ over xGMI, $S(g-1)/g$ over the fabric):

| $g$ | $T_\mathrm{intra}/T_\mathrm{inter}$ | sequential | overlapped | ceiling on the saving |
|---|---|---|---|---|
| 4 | 0.49 | 1.49 | 1.00 | 33 % |
| 64 | 0.37 | 1.37 | 1.00 | 27 % |
| 576 | 0.37 | 1.37 | 1.00 | **27 %** of exchange time |

Part of this exists already: `comm_layered` carries `inflight = 2`, so one exchange's
intra stage runs while the previous one's inter stage is on the wire. Two limits are
suspected and must be measured rather than assumed: the overlap is at *whole-exchange*
granularity, not per slab, so it needs two exchanges pending to engage at all; and the
inter transport takes one exchange at a time, which serialises the very stage the overlap
depends on. The experiment has three parts:

1. **Measure the present overlap.** `DIST_STATS` extended to report xGMI-busy,
   fabric-busy and both-busy time separately, at 2, 3 and 4 real nodes. This says how
   much of the 27 % is already collected.
2. **Deepen the pipeline**: overlap at slab granularity rather than exchange granularity,
   and allow more than one inter exchange in flight. Gate: `t_dist` identical at every
   size, the both-busy fraction, and the exchange time.
3. **xGMI as a relief valve for NIC imbalance.** With the general (non-power-of-two) map
   at 576 and with the `alltoallv` redistribution, per-peer slab sizes differ, so some
   NICs finish early. An APU whose NICs are idle could forward a peer APU's inter-node
   traffic over xGMI — multi-rail striping through the node's aggregate 400 GB/s rather
   than each APU's own 100. This cannot raise the total bytes leaving the node, so its
   ceiling is only the *imbalance*; measure the imbalance first (part 1) and implement
   only if it is material.

**What is testable here and what is not.** aac6's 1 GbE makes the ratio nothing like the
target's, so the *absolute* numbers must wait. What can be established now: the
both-busy fraction at 2--3 real nodes, whether the one-at-a-time inter stage is a real
limit, and that a deeper pipeline is bit-identical. The model is then re-run with the
target's constants. Confidence that the mechanism helps: high. Confidence in the 27 %:
medium — it is an upper bound assuming perfect overlap and no contention between the
push kernel and the NIC DMA for HBM bandwidth, which is itself worth measuring (H4, H7).

**H8 Multi-NIC endpoint structure** (target only): two contexts per APU thread, one per
NIC, with the slab exchange striped. Cannot be measured here, but the structure is built
and switched now so the target's first session is a measurement, not a port.

### 29.5 Order, and why

1. **E1, E2** — no node time, and they bound what the rest can achieve.
2. **E0** — the decisive micro-benchmark; it decides E4 and E5 before either is written.
3. **H2, H3, H4, H7** — kernel-level measurements that may change the constants every
   later experiment is judged against. Cheap, and they belong before the pipeline work.
   **E9 part 1** (measuring the present xGMI/fabric overlap) belongs here too: it needs
   only two real nodes and it decides whether E9 parts 2 and 3 are worth writing.
4. **E3** — the largest expected single win; also the one that changes the memory budget
   every other experiment operates within.
5. **E4, E5** — the K4 exploitation, informed by E0 and E3.
6. **E6, E7, E8** — independent gains, any order.
7. **H1, H5, H6** — opportunistic; H1 needs an administrator.
8. **H8** — built now, measured on the target.

### 29.6 Discipline

Every measurement: five runs, reference evicted, one node per series, standard deviation
quoted, digits compared. Every configuration behind a switch, defaults unchanged until
the user adopts. A Pareto table of (wall, node memory, modelled 576-node ceiling) is
maintained across the whole campaign so that the final choice is made on one page, and
`RESULTS.md` §78 records every point including the losers.

**Expected outcome**: the empirically best design point, with the evidence for why it
beats the alternatives, and a number for what the K4 structure is worth when exploited
fully rather than incidentally.
