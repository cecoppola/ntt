# bench/ — one question per program

Build with `make` (after `module load rocm`); run through `../suite`. Each
program prints a human table plus `META` and `RESULT` lines. Full results and
verdicts: `../RESULTS.md` (section given below).

| program | question | RESULTS.md | status |
|---|---|---|---|
| `01_butterfly` | which arithmetic engine? | §2 | current |
| `02_capacity` | how much memory, at what bandwidth, which allocator? | §4, §24 | current |
| `03_fabric` | corner turn: allocator, push vs pull | §4 | current |
| `old/04_ntt_lds` | simple LDS NTT | §3 | superseded by 06 (`make old`) |
| `old/05_ntt_reg` | register-blocked NTT | §3 | superseded by 06 (`make old`) |
| `06_ntt_opt` | XOR swizzle, twiddle ablation | §3, §30 D13 | current |
| `07_ntt_tw` | twiddle traffic variants | §3, §30 D13 | current |
| `old/08_multiply` | first complete multiply | §6 | superseded by 13 (`make old`) |
| `09_logic` | per-op VALU cost and true latency at the measured clock | §9, §24, §37 | current |
| `10_lds` | LDS bandwidth, bank conflicts, shuffles | §10 | current |
| `11_fabric_ntt` | fabric width / chunk / stride / overlap | §11 | current |
| `12_ntt_inv` | register-blocked inverse | §13 | current |
| `13_multiply2` | the production multiply | §14, §24 | current |
| `14_sustained [s] [GiB/APU]` | sustained throughput, resident footprint | §23, R8 | current |
| `15_barrett_f64 [Mpairs]` | the paper's FP64 modmul: exactness, corrections, rate, D1 variants | §19, §30 D2, §31 D1 | campaign 4 |
| `16_ntt_tile [maxlog] [testlog]` | the paper's tiled DIF NTT; twiddle modes (D4); template kernels (D5) | §25, §30 D11, §32, §33 | campaign 4 |
| `17_hostreg [GiB] [bigGiB]` | `hipHostRegister` by NUMA placement | §20 | campaign 4 |
| `18_staging` | kernels on pinned host staging | §26 | campaign 4 |
| `19_peer_gather [GiB]` | 4-way peer gather (GPU CRT) | §21 | campaign 4 |
| `20_cpu [log2 n]` | CPU CRT in five variants (D8), repack, STREAM, seeds, interference | §27, §34 | campaign 4 |
| `21_alloc [maxGiB]` | allocation costs; calloc on reuse | §22, §30 D10 | campaign 4 |
| `22_clock` | shader clock during bursts | §24, §29 | campaign 4 |
| `23_d2h` | D2H blit vs streams/chunks/targets | §30 D7 | campaign 4 |
| `mem/infcache` | gather + pointer chase vs working set | §37 | campaign 4 |
| `arith/mfma` | int8 / bf16 / f64 matrix-core rates | §37 | campaign 4 |

Subdirectories `arith/ mem/ lds/ fabric/ kernel/ sustained/ system/` hold
new work by category (PLAN.md §11); `old/` holds superseded programs.

Shared code: `common_ntt.h` (harness, `META`/`RESULT`), `ntt_kernels.h`
(primes, butterflies, tables, hash61). Arguments for `../suite` go in
`<name>.args`. Instruction counts: `make isa B=<name> K=<kernel>` (`../isa.py`).

Rules every new program follows: `../PLAN.md` §12.
