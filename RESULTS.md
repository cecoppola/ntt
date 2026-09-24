# Measured results — benchmark campaign 1

Everything here was measured on `PPAC_MI300A_SPX` (4 × MI300A). Sources are
`bench/01`–`bench/08`, all plain C built with `hipcc -x hip`. Several of these
overturn recommendations in `DESIGN.md`; those are marked **CORRECTION**.

## 1. Where the design stands

| Milestone | Status |
|---|---|
| Arithmetic engine chosen on evidence | done (bench/01) |
| Memory capacity established | done — 460 GiB at 14.0 TB/s (bench/02) |
| Corner-turn method chosen | done — push, 697 GB/s (bench/03) |
| NTT kernel, verified + optimised | done — 2 325 Gbfly/s, 60 % of bound (bench/04–07) |
| **Full big-integer multiply, verified** | **done — 1.98 ms per 60.6 M-digit product on 1 APU (bench/08)** |
| Binary splitting / division / radix conversion | not started |

The multiply is verified two ways: against schoolbook at small operand sizes,
and at full size by the homomorphism `hash61(a)·hash61(b) = hash61(a·b)` over
2<sup>61</sup>−1 — which is also the production error detector (§8 of DESIGN.md).

## 2. Arithmetic engine (bench/01)

| Engine | Gbfly/s node | log₂p | **Gbit-bfly/s** |
|---|---:|---:|---:|
| **`shoup_u64`** Harvey Alg. 4, p<2<sup>62</sup> | 3 886 | 62 | **240 949** |
| `mont_u64` Harvey Alg. 5 | 3 114 | 62 | 193 038 |
| `gold_u64` 2<sup>64</sup>−2<sup>32</sup>+1 | 2 713 | 64 | 173 653 |
| `shoup_f64` FP64 fma, p<2<sup>50</sup> | 4 212 | 50 | 210 602 |

FP64 issues the most butterflies and still loses: each carries 19 % fewer bits,
and three 50-bit primes are needed where two 62-bit primes suffice. Shoup
compiles to **28 VALU instructions per butterfly at 88 % issue efficiency**;
`v_mad_u64_u32` and `v_mul_lo_u32` are *not* quarter-rate on CDNA3.

## 3. The NTT kernel: 1 554 → 2 325 Gbfly/s

| Variant | Gbfly/s node | % of register bound |
|---|---:|---:|
| LDS radix-2, one read-modify-write + barrier per stage | 1 554 | 40 % |
| register-blocked, 8 points/thread, 3 exchanges not 11 | 2 211 | 57 % |
| **+ XOR swizzle on the LDS exchanges** | **2 325** | **60 %** |
| (twiddles ablated — an upper bound, wrong math) | 2 817 | 72 % |

**Register blocking is the big win (1.42×).** Holding 8 points per thread makes
each group of 3 radix-2 stages register-resident, so LDS is only a transpose
buffer between groups: 3 round trips and 3 barriers instead of 11 of each.

**CORRECTION to DESIGN.md §4.4 on the XOR swizzle.** The blanket
recommendation was wrong in both directions. In the *simple* LDS kernel the
access pattern is already conflict-free and the swizzle costs 3 %. In the
*register-blocked* kernel it is essential — counting bank classes (an 8-byte
element occupies banks 2e, 2e+1 of 32, so its class is e mod 16) shows three of
the six exchange phases collapse 64 lanes onto 4 or 2 classes:

```
  A->B write/read, B->C write   16 classes   (the 8-byte floor)
  B->C read,  C->D write         4 classes   16-way conflict
  C->D read                      2 classes   32-way conflict
```

`e ^ ((e>>4)&15)` restores all six to 16 and is worth 5 %, plus it cuts the
APU-to-APU spread from 12.4 % to 1.8 %. The lesson is that the swizzle is a
property of the *access pattern*, not of the kernel, and must be derived.

**Twiddle traffic costs 21 %.** Per block (N=2048, 16 KiB of data) each thread
loads 27 distinct twiddles held as separate `w[]`/`wp[]` arrays = 54 loads,
432 B, so **108 KiB of twiddles per 16 KiB of data**. The table is 32 KiB —
exactly the L1 — and 4 blocks/CU stream 128 KiB of data through that same L1.

Three fixes tried (bench/07); only the first works:

| | effect |
|---|---|
| interleave `w`,`wp` into one `ulong2` load | **+6 %**, keep |
| non-temporal loads/stores for the data | **0.66× — do not use** |
| stage the hot 8 KiB of table in LDS | 0.96×, occupancy loss exceeds the saving |

**CORRECTION to DESIGN.md §12/report guidance on non-temporal loads.** The node
report measured 1.10× for streaming reads, but that does not transfer: with the
strided 8-element-per-thread load pattern, `__builtin_nontemporal_load` defeats
coalescing and costs 34 %.

## 4. Capacity and fabric (bench/02, bench/03)

* **460 GiB allocated and first-touched with `hipHostMalloc`**, read at
  14.0 TB/s and triad 11.8 TB/s — full HBM speed, and it defeats the
  96 GiB/APU `amdttm.pages_limit` device cap. Cost: 63 s to allocate plus 40 s
  to first-touch (worst APU, all four concurrent). Allocate once, never free.
* **all-to-all push 697 GB/s vs pull 399 GB/s** — a hand-written kernel doing
  remote *stores* beats remote loads by 1.75× and beats `hipMemcpyPeerAsync`
  (418 GB/s in the node report) by 1.67×.
* **The allocator does not affect fabric bandwidth.** `hipMalloc`,
  `hipHostMalloc` coherent and non-coherent agree to within 1 % on every
  pattern. An early reading of 66 GB/s on host memory was a scalar grid-stride
  kernel, not an allocator penalty — the same buffers reach 89 GB/s single-flow
  with 128-bit accesses.

## 5. CORRECTION: the fabric is not the dominant cost

DESIGN.md §1 ranked xGMI as the second constraint and §4.6 concluded "the
fabric costs more than the arithmetic and the HBM combined". That is true of a
single distributed multiply but false of the run, because **only level 0 of the
recursion has to be distributed**:

```
  level 0:  1 merge,  3 planes = 165.0 GiB  -> must distribute
  level 1:  2 merges, 3 planes =  82.5 GiB  -> fits one APU
  level 2:  4 merges, 3 planes =  41.3 GiB  -> fits one APU
```

Fabric is paid on roughly 9 full-size multiplies (level 0, the Newton
iteration, the top of the scaled remainder tree) — about **21 % of the run**.
The NTT kernel is the dominant term after all, which is why §3 above was worth
the effort.

## 6. The full multiply (bench/08)

L = 2048 × 2048 = 4.19 M points, 48 bits/point, two 62-bit primes.

| | value |
|---|---:|
| product capacity | 60.6 M decimal digits |
| time, 1 APU | **1.98 ms** |
| workspace | **101 MB = 1.66 bytes per decimal digit** |
| effective rate | 140 Gbfly/s on 1 APU (kernel alone: ~390) |

Three things mattered, in order:

1. **Never use `__uint128_t % p` in a kernel.** The twiddle, pointwise and
   scale passes each did a software 128-bit division. Replacing them with two
   Shoup multiplies (twiddle, using the two-level table so no general modular
   multiply is needed) and Montgomery REDC (pointwise, where both operands
   vary) was worth **3.1×** — 5.85 ms to 1.92 ms. The Montgomery 2<sup>−64</sup>
   folds into the final 1/L scaling so nothing else has to know.
2. **In-place transpose halves the workspace.** N1 = N2, so tiles (i,j) and
   (j,i) can be swapped through LDS with no second plane. Six planes become
   three: 3.32 → **1.66 bytes/digit**, for 3 % more time. Since memory capacity
   sets the digit count and time does not bind, this is the best trade in the
   whole design.
3. **The four-step index decomposition is easy to get wrong.** Bailey's first
   transform must run over the index with stride n₂. The row kernel transforms
   the *contiguous* index, so points must be stored **column-major**: memory
   offset `o = r*N2 + c` holds point `c*N1 + r`. Storing them row-major
   computes the DFT of a permuted sequence, which is not convolution-
   compatible. This was the first bug and it fails silently without a
   reference check.

Still on the table: 12 of the 16 passes per multiply are glue (split, twiddle,
transpose, pointwise, scale). Fusing the twiddle into the row kernel's
epilogue, the split into its prologue, and the pointwise into the last forward
transform should approach the ~0.93 ms roofline.

## 7. Calibration against known implementations

| Implementation | Digits | Memory | Bytes/digit |
|---|---:|---:|---:|
| y-cruncher, π, CPU | 5 × 10<sup>9</sup> | 24 GB | 4.80 |
| y-cruncher, π, CPU | 1 × 10<sup>10</sup> | 48 GB | 4.80 |
| gpupi, π, RTX 5090 (96-bit NTT) | 1 × 10<sup>9</sup> | 6.14 GB | 6.14 |
| reference point, e, 4 × MI300A | 4 × 10<sup>10</sup> | 460 GiB | 12.35 |
| **this work, transform workspace only** | — | — | **1.66** |

gpupi computes 10<sup>9</sup> digits of π in 4.1 s on one RTX 5090 using three
32-bit primes (96 bits, ≈2.75 bits per stored byte — nearly the same density as
two 62-bit primes at 2.81) but is capped at 2<sup>28</sup> limbs. The 62-bit
primes chosen here, with 2<sup>40</sup>·3·5·7 dividing p−1, scale about 4000×
further and keep 7-smooth transform lengths for fine size control.

The 1.66 figure covers the transform domain only; the packed operands, the
binary-splitting stack and the radix-conversion power table are extra. A
whole-program figure of 3–5 bytes/digit looks reachable, which against 460 GiB
puts the target at **1.0–1.5 × 10<sup>11</sup> digits**.

## 8. Next

1. Fuse the glue passes into the row kernels (≈2× on the multiply).
2. Squaring path and transform reuse — the binary-splitting merge multiplies by
   the same Q₂ twice.
3. Multi-plane, multi-APU multiply for level 0 using the push corner turn.
4. Arena allocator with `size()`/`space()` bounds, then binary splitting.
5. Deferred lazy reduction: a p<2<sup>61</sup> prime allows one conditional
   subtract per 3-stage register group instead of per butterfly, worth ~8 % for
   2.2 % density — take it only if time ever starts to bind.

---

# Measured results — benchmark campaign 2: the hardware units

Campaign 1 measured composite kernels. This measures the individual logic,
memory and interconnect units against exactly the operation mix an optimised
NTT uses, so the kernel numbers can be predicted rather than just observed.
Sources: `bench/09_logic.c`, `bench/10_lds.c`, `bench/11_fabric_ntt.c`.
One APU unless stated; `cyc` means lane-cycles at 228 CU × 64 lanes × 2.10 GHz
= 30.64 Tlane-cycles/s.

## 9. Logic units (bench/09)

| operation | Gop/s | **cyc/op** | ILP needed |
|---|---:|---:|---:|
| `v_mad_u64_u32` (32×32+64) | 20 495 | **1.50** | 1.9 |
| `mul_lo64` (64×64→lo) | 7 169 | 4.27 | 1.3 |
| `mul_hi64` (`__umul64hi`) | 3 887 | **7.88** | 1.2 |
| `sel64` (lazy conditional subtract) | 6 277 | **4.88** | 1.3 |
| `add64` / `sub64` | 21 806 / 12 113 | 1.41 / 2.53 | — |
| `bfe64`, `shift64` (index math) | 12 050 / 5 254 | 2.54 / 5.83 | 1.9 / 1.0 |
| **`shoup` modmul** | 2 021 | **15.17** | 1.2 |
| `mont` modmul | 1 204 | 25.46 | 1.1 |
| **full butterfly** | 1 107 | **27.67** | 1.0 |
| `mul_lo32` / `mul_hi32` | 22 047 / 20 069 | 1.39 / 1.53 | 1.2 / 2.1 |
| `sel32`, `xor32`, `brev32` | 14 094 / 24 833 / 14 416 | 2.17 / 1.23 / 2.13 | — |
| `fma64` / `dmul64` / `rint64` | 22 183 / 22 088 / 10 155 | 1.38 / 1.39 / **3.02** | 2.1 |

Four consequences:

1. **`v_mad_u64_u32` is a 1.5-cycle instruction** — not quarter-rate. This is
   why the 64-bit Shoup path is viable at all: a 64×64→lo costs 4.27 cyc and a
   →hi costs 7.88, so the whole modmul is 15.17.
2. **The lazy conditional subtract costs 4.88 cyc — 18 % of a butterfly.**
   Deferring it to one per 3-stage register group (possible with a
   p < 2<sup>61</sup> prime, which loses only 2.2 % of density) is worth
   **11.8 %**, not the 8 % estimated in DESIGN.md §4.1.
3. **The VALU is fully pipelined for this mix**: latency/throughput is 1.0–2.2
   for every operation, and 1.0 for the butterfly itself. The NTT kernel is
   **issue-bound, not latency-bound**, so register blocking buys instruction
   count, never overlap — and there is no reason to hold more than a couple of
   independent chains.
4. **A 32-bit-prime engine is not worth building.** Reconstructing from the
   primitives at L = 2<sup>28</sup>:

   | primes | bits/point | cyc/point/stage | bits/cyc | bits/byte |
   |---|---:|---:|---:|---:|
   | 2 × 62-bit | 45 | 55.3 | 0.813 | **2.81** |
   | 3 × 31-bit | 30 | 26.9 | **1.116** | 2.50 |
   | 4 × 31-bit | 30 | 35.8 | 0.837 | 1.88 |

   Three 31-bit primes are 1.37× better on compute but 11 % worse on memory,
   and memory is the binding constraint. **2 × 62-bit confirmed on both axes.**

`rint64` at 3.02 cyc is the only FP64 operation that is not ~1.4; it is 30 % of
an FP64 modmul, which explains why the FP64 butterfly in bench/01 was only
1.08× the integer one despite a cheaper reconstruction (9.94 vs 15.17 cyc).

> The `add64`/`sub64`/`add32` *latency* figures are invalid — the compiler
> strength-reduces a dependent add chain. Throughput for those is sound; the
> multiply, select and composite figures cannot be reduced and are trustworthy.
> The latency column generally is per resident wave-slot, so true latency is
> about 4× larger at 4 waves/SIMD; the *ratio* is what matters and is unaffected.

## 10. LDS and cross-lane units (bench/10)

Read+write bandwidth on one APU, against a ~61.3 TB/s ceiling (128 B/clk/CU):

| access width | GB/s | % of ceiling |
|---|---:|---:|
| 32-bit `ds_b32` | 27 442 | 45 % |
| 64-bit `ds_b64` | 39 787 | 65 % |
| **128-bit `ds_b128`** | **45 359** | **74 %** |

Bank conflicts follow the class model exactly — an 8-byte element occupies
banks 2e, 2e+1 of 32, so its class is `e mod 16`:

| stride | classes | GB/s | vs stride 1 |
|---:|---:|---:|---:|
| 1 | 16 | 39 859 | 1.00× |
| 2 | 8 | 29 034 | 0.73× |
| 4 | 4 | 15 844 | 0.40× |
| 8 | 2 | 8 068 | 0.20× |
| 16 | 1 | 4 057 | **0.10×** |
| 17 | 16 | 39 083 | 0.98× |
| 32 | 1 | 4 059 | **0.10×** |
| 33 | 16 | 39 454 | 0.99× |

This is the quantitative justification for the swizzle work in §3: the
register-blocked kernel's `B->C read` had 4 classes (0.40×) and its `C->D read`
had 2 (0.20×). Odd strides are free, which is why 17 and 33 recover fully.

| cross-lane / sync | cost |
|---|---:|
| `__shfl_xor` 32-bit (`ds_bpermute`) | 6.24 cyc |
| `__shfl_xor` 64-bit | 12.18 cyc |
| one LDS 64-bit write **plus** read | 12.32 cyc |
| `__syncthreads` | 6.81 cyc |
| LDS load-to-use | 4.32 cyc/wave-slot (≈17 cyc absolute) |

**Cross-lane shuffles are not a win.** A 64-bit shuffle costs the same as a
whole LDS write+read pair, and an 8-register-per-lane transpose needs several
shuffle rounds rather than one exchange. The Sugizaki "eliminate shared memory"
result does not transfer to CDNA3 at this shape. LDS with the XOR swizzle is
the right structure. Barriers are only 3.4 % of a transform, so removing them
was never the opportunity it looked like.

## 11. Interconnect (bench/11)

**CORRECTION — access width across the fabric behaves opposite to local memory.**
Local LDS and HBM both prefer wide accesses; the fabric prefers narrow ones:

| all-to-all push | node GB/s |
|---|---:|
| **64-bit stores** | **909** |
| 128-bit stores (what bench/03 used) | 699 |
| 256-bit stores | 368 |

Stable to within 1 % at 4 GiB and 8 GiB buffers. So the corner turn should use
**64-bit** remote stores: 1.30× over bench/03's choice and **2.17× over
`hipMemcpyPeerAsync`** (418 GB/s in the node report). The mechanism is not
isolated — plausibly more outstanding requests per link — and is worth knowing
before anyone "optimises" a transfer kernel by widening it.

**CORRECTION — there is no minimum message size for a push kernel.**

| contiguous run | node GB/s |
|---|---:|
| 128 B | 720 |
| 512 B … 1 MB | 720–723 |

Flat across four orders of magnitude. The node report's guidance to batch
xGMI messages above ~256 KiB is a property of `hipMemcpy`'s ~10 µs fixed cost,
not of the fabric. A corner turn built from kernel stores can use whatever tile
size the transform wants — the four-step decomposition is unconstrained here.

Scattered stores still collapse, as expected:

| stride (×16 B) | node GB/s | vs contiguous |
|---:|---:|---:|
| 1 | 699 | 1.00× |
| 2 | 227 | 0.33× |
| 4 | 137 | 0.20× |
| 16 | 128 | 0.18× |

**CORRECTION — the corner turn does overlap with compute.**

```
  corner turn alone     18.70 ms
  butterflies alone      3.77 ms
  concurrent            19.67 ms      (serial would be 22.46)
  overlap efficiency        74 %
```

DESIGN.md §7.5 said not to expect fabric transfers to hide behind computation,
on the strength of the node report's 16 % figure. That figure is for
`hipMemcpyPeerAsync`, which is serviced by a blit kernel occupying CUs. A push
kernel on its own stream overlaps at **74 %**. This answers open question 1 and
means level-0 fabric time largely disappears behind the butterflies rather than
adding to them.

## 12. Revised model

| term | campaign 1 | campaign 2 |
|---|---:|---:|
| corner turn, L = 2<sup>33</sup>, 6 turns | 443 ms @ 697 GB/s | **340 ms @ 909 GB/s, ~74 % hidden** |
| butterflies, same | 312 ms | 312 ms, −11.8 % if reductions are deferred |
| lazy-reduction saving | est. 8 % | measured 11.8 % |
| shuffle-based exchange | untested | rejected, no faster than LDS |
| four-step tile size | assumed ≥256 KiB | unconstrained |

The fabric was already demoted from second constraint to ~21 % of the run in
§5; with 1.30× more bandwidth and 74 % overlap it is close to free. **The NTT
kernel's instruction count is the whole game**, and after the deferred
reduction the next targets are the 272 `v_mov_b32` (18 % of issue) and the 147
`s_nop` (10 %) identified in §3.

---

# Measured results — campaign 3: the fast multiply

Campaign 2 showed the NTT kernel's instruction count is the dominant term.
This campaign built the missing inverse kernel, folded the glue into the
transforms, and then measured where the time actually goes — which closed the
optimisation question rather than opening another round of it.
Sources: `bench/12_ntt_inv.c`, `bench/13_multiply2.c`.

## 13. Register-blocked inverse (bench/12)

The forward transform was register-blocked in campaign 1; the multiply still
ran the simple LDS kernel in both directions because no register-blocked
Gentleman-Sande inverse existed. The inverse runs the stages in the opposite
order (h = 1, 2, 4, …, 1024) so it needs the mirror set of layouts. In a layout
`{base + s*k}` with k in [0,8), a stage of half-size h is register-resident iff
h ∈ {s, 2s, 4s}:

| group | stride | stages |
|---|---:|---|
| G1 | 1 | h = 1, 2, 4 |
| G2 | 8 | h = 8, 16, 32 |
| G3 | 64 | h = 64, 128, 256 |
| G4 | 256 | h = 512, 1024 |

All four are bijections on [0,2048); three exchanges, three barriers. Two of
the six exchange phases have 32-way and 8-way conflicts, which the XOR swizzle
returns to the 16-class floor — worth 0.10× and 0.73× respectively by §10.

| inverse kernel | Gbfly/s node |
|---|---:|
| simple LDS | 1 522 |
| **register-blocked** | **2 569** |

**1.69×**, better than the forward's 1.50×. Verified two ways: identical output
to the simple kernel, and `fwd_reg → inv_reg → ×1/N` is the identity.

## 14. The fused multiply (bench/13)

Both fast kernels, plus the glue folded in: the operand split into the first
forward transform's prologue, the four-step twiddle into its epilogue, the
inverse twiddle into the last inverse transform's prologue and the 1/L scaling
into its epilogue. Passes per prime drop from 30 plane-touches to 20.

| | bench/08 | **bench/13** |
|---|---:|---:|
| 60.6 M-digit product, 1 APU | 1.98 ms | **1.23 ms** |
| speedup | — | **1.61×** |
| workspace | 1.66 B/digit | 1.66 B/digit |

Verified against schoolbook at small operand sizes and by the hash homomorphism
at full size, as before.

### Where the time goes

Per prime, one pass (fwd1, transpose and fwd2 each run twice per multiply):

| kernel | ms | share |
|---|---:|---:|
| `fwd1` split + NTT + twiddle | 0.121 | 30.2 % |
| transpose (in place) | 0.034 | 8.5 % |
| `fwd2` NTT | 0.060 | 14.9 % |
| pointwise (Montgomery) | 0.032 | 8.1 % |
| `inv1` NTT | 0.046 | 11.4 % |
| transpose | 0.032 | 8.0 % |
| `inv2` twiddle + NTT + scale | 0.075 | 18.8 % |

Reading this against the isolated kernels: `fwd1` = 0.060 (the NTT) + 0.032
(the split) + 0.029 (the twiddle), and `inv2` = 0.046 + 0.029. So

* **79 % of the multiply is now inside the NTT row kernels**, running at ~66 %
  of the register-resident butterfly bound — the same fraction campaign 1
  measured for the kernel alone. The pipeline no longer wastes anything
  significant.
* The transposes are only 16 % and the pointwise 5 %. Fusing the transpose into
  the next transform's load would replace two coalesced plane-touches with a
  stride-2048 read (one useful element per 64-byte line), so it would lose.
* The scattered split costs only 0.032 ms despite reading one cache line per
  lane, because the packed operand (25 MB) fits in the 256 MB Infinity Cache.
  A coalesced split as a separate pass plus a transpose would cost more.

## 15. Rejected: deferred lazy reduction

Campaign 2 measured `sel64` at 4.88 cyc, 18 % of a butterfly, and noted that a
smaller prime would let the lazy range grow so the conditional subtract could
be dropped. Working it through:

The forward butterfly gives `X' = U+V`, `Y' = U-V+2p` with V in [0,2p), so the
range grows by 2p per stage and 11 stages from [0,p) reach [0,23p). That fits a
64-bit word only for **p < 2<sup>59</sup>**, at which point every select in the
forward transform can go.

| | value |
|---|---|
| saving | 17.6 % of butterfly time, forward kernels are 59 % → **10.4 % overall** |
| cost at L = 2<sup>33</sup> | b drops 45 → 42 bits/point, a **6.7 % density loss** |

**Rejected.** 10.4 % faster for 6.7 % fewer digits is a bad trade when memory
binds and time does not. My campaign-2 note called this an 11.8 % win; that
looked only at the speed side of the ledger. It is also moot for the inverse:
Gentleman-Sande's `X = U+V` *doubles* the range each stage rather than adding to
it, reaching 2<sup>11</sup> after 11 stages, so it would need p < 2<sup>53</sup>
and cannot use the technique at any useful density.

This is the shape of every remaining kernel optimisation: the butterfly is
issue-bound (§9), the instruction count is near what the ISA allows (28 of 34.7
are irreducible), and the cheap tricks all buy speed with density. **The kernel
work is finished until something changes the memory ledger.**

## 16. Where the project stands

| | |
|---|---|
| multiply | verified, 1.23 ms per 60.6 M-digit product on 1 APU |
| transform workspace | 1.66 bytes/digit |
| scaled to D = 10<sup>11</sup> | top-level multiply ≈ 0.8 s on 4 APUs |
| next | variable transform length, then binary splitting |

`bench/13` is hard-wired to L = 2048². Real use needs L from about
2<sup>16</sup> to 2<sup>34</sup>, chosen 7-smooth to fit the operand within a
few percent (§4.3 of DESIGN.md). After that, binary splitting is what turns the
3–5 bytes/digit whole-program estimate into a measurement.

---

# Measured results — campaign 4: reproducing the *e*-to-40-billion paper

Plan: `PLAN.md`. Node for this campaign: **`ppac-pl1-s24-16`** unless stated.

## 17. Phase 0 — environment (2026-09-12)

Source: `envcheck.sh` → `results/0_env_ppac-pl1-s24-16.txt`; `tests/t_params.c`
→ `results/0_params.txt`.

| item | value |
|---|---|
| node | `ppac-pl1-s24-16`, kernel 6.8.0-134-generic |
| ROCm | 7.2.4, HIP 7.2.53211, clang 22.0.0git, amdgpu 6.19.14, AMDSMI 26.2.2 |
| GPUs | 4 × gfx942, 228 CU, 2100 MHz max sclk, 128 GiB VRAM each, `xnack-` |
| CPU | 4 sockets × 24 Zen4 cores, 192 threads, L3 384 MiB (12 × 32 MiB) |
| NUMA | 4 nodes (one per APU), 128 GB each, distance 10 / 32 |
| memory | 502 GB total, 495 free, no swap; THP `always`; no hugetlb reserved |
| **`ulimit -l`** | **32 970 408 KB ≈ 31.4 GiB** — below the paper's 64 GB `hipHostRegister`; B3 decides whether the KFD userptr path honours it |
| `ulimit -m` | 471 859 200 KB = 450 GiB (Slurm RSS cap) — fine for 256 GB |
| `amdttm.pages_limit` | 134 217 728 pages = **512 GiB**, not the 96 GiB/APU DESIGN.md §5.1 assumed (CORRECTION: on this node `hipHostMalloc` was never needed to defeat a cap; it remains the right choice for bandwidth) |
| idle telemetry | sclk 94 MHz, fclk 2000, mclk 1300, ~110 W and 45 °C per APU |

Paper constants, all verified with GMP (`t_params`):

| check | result |
|---|---|
| P[0..3] prime, < 2⁵², 2³³ \| p−1 | OK |
| g = 3, 5, 3, 10 are generators | OK |
| ω₃₃ = g^((p−1)/2³³) has exact order 2³³ | OK — 678007507195576, 3536920527846901, 1501474000275416, 2276121144993249 |
| μ₁₁₅ = ⌊2¹¹⁵/p⌋ | 10588265656658394641, 11406682325772086755, 14485786757268845297, 16062471030168855059 (64-bit each) |
| Σ log₂ p | 206.04 |
| convolution bound b = 64 at n = 2³² and 2³³ | 160, 161 ≤ 206 OK |
| LEAF μ = ⌊2¹²³/10¹⁸⌋ | 10633823966279326983 matches the paper |
| bench primes p1, p2: prime, 2⁴⁰ \| p−1, 11 a generator | OK |
| proposed tier-1 primes (paper does not list its 8) | first 8 primes above 2⁶²: 4611686018427388039, …073, …081, …091, …093, …097, …157, …181 |

## 18. Harness (2026-09-12)

`common_ntt.h` now emits `META …` once per program (host, HIP version, ROCm
path, max sclk, device count, date) and `RESULT <bench> <metric> <unit> <node>
<apu…>` for every `report_sum`/`report_max`. `./suite [-w node] [-t time]
[bench…]` builds, allocates one node, runs each program as an srun step with
an `amd-smi` JSON sampler alongside, and writes
`results/<date>_<node>/{<bench>.log,<bench>.smi,env.log,results.tsv}`.
`./diff A B` prints per-metric ratios. Per-program arguments live in
`bench/<name>.args`.

Shared primitives (primes, `smul`, `FB/FWD3/GB/GINV3/EXW/EXR`, table builders,
`hash61`) moved to `bench/ntt_kernels.h`; 13 and 14 use it. Re-verified on
`ppac-pl1-s24-16` (`results/20260912_1704_ppac-pl1-s24-16/`):

| program | check | rate |
|---|---|---|
| 13_multiply2 | hash61 OK, schoolbook OK | **1.08 ms**/multiply (§14 recorded 1.23) |
| 14_sustained | trivial == baseline OK | 2 241 Gbfly/s baseline; 2 339 sustained (§R8: 2 340) |

## 19. B1 — the paper's FP64-Barrett modmul (`bench/15_barrett_f64`)

10⁹ random pairs per prime per APU plus all pairs of edge values, against a
128-bit integer reference; rates with 8 independent chains.

| inputs | two corrections | one correction wrong |
|---|---|---|
| a, b ∈ [0, p) | exact, all primes | never |
| **a ∈ [0, 2p), b ∈ [0, p)** — lazy value × canonical twiddle, the kernel's case | **exact, all primes** | P0 0.369 %, P1 0.214 %, P2/P3 never |
| a, b ∈ [0, 2p) | **wrong** 0.37 % (P0), 0.21 % (P1); exact P2/P3 | 4.4 % / 3.7 % / 0.07 % / 0.05 % |

**Q4 resolved.** The two corrections are needed only because one operand is
lazy: with p ≈ 2⁵² and a < 2p, hi·pinv reaches 2⁵³ where the ulp is 1 and q
can be off by 2. The paper's 0.57 % for P[1] is this effect (their operand
distribution differs from uniform). **Both operands lazy is unsafe for P0 and
P1** — hi·pinv reaches 2⁵⁴, ulp 4 — so the pointwise product must
canonicalise one side first, which is exactly what the paper's
`modmul(canon(a[i]), pw[i])` does. Phase 3 rule: never feed two lazy values to
this modmul.

| rate, per APU (node) | Gop/s |
|---|---:|
| paper's FP64 Barrett modmul | **1 306–1 337 (5 281–5 319)** — paper: ~775 |
| Shoup u64 with correction, same primes | 1 255–1 289 (5 072–5 115) |
| paper's DIF butterfly, lazy [0,2p) | 995–1 018 Gbfly/s (4 032–4 050) — bench/01 FP64: 4 212 |

The paper's modmul runs at **1.72× its stated rate** on this node. "12.4 TF64"
in the paper is therefore not the hardware ceiling; our number is 21.3 TF64
in modmul-equivalent ops, still under half of bench/09's `fma64` rate — the
`floor`/`rint` and select steps are the cost, as bench/09 found. Shoup u64
is no faster once its correction is included (bench/09's 2 021 was without).

## 20. B3 — `hipHostRegister` staging (`bench/17_hostreg`), 16 GiB per APU

| placement | register | kernel read | kernel write | D2H `hipMemcpy` |
|---|---:|---:|---:|---:|
| `hipHostMalloc` (bench/02 baseline) | 4.7 s | 3 744 GB/s | 3 211 | 58.5 |
| **registered, first-touched on own NUMA node** | 2.5–4.7 s | **3 706** | **3 246** | 58.5 |
| registered, touched on the next node | 2.5–4.6 s | **67.5** | 93.3 | 47.6–58.4 |
| registered, MPOL_INTERLEAVE over 4 nodes | 2.6–4.9 s | 144 | 362 | 55 |
| single 64 GiB block | **OK, 4.8 s** | — | — | — |

Per APU; node totals ×4. Registration serialises across threads (2.5, 3.3,
4.0, 4.7 s).

* **Gate passes.** `ulimit -l` (31.4 GiB) is not enforced on the KFD userptr
  path; 4 × 16 GiB and one 64 GiB registration both succeed.
* **Registered memory on the right NUMA node is HBM-speed**, identical to
  `hipHostMalloc`. Nothing is lost by the paper's malloc + register design.
* **The wrong NUMA node costs 55×** (67 GB/s read). Interleave costs 26×
  (144). This is the paper's "+20 s for NUMA interleave" mechanism, and it
  means the first-touch threads must be pinned to the APU's node — OpenMP
  thread placement is not enough on its own.
* **`hipMemcpy` D2H is 58.5 GB/s per APU regardless of placement** — the blit
  engine, ~60× slower than a kernel store into the same memory. The paper's
  "4-thread D2H → hstage_buf" of a 16 GiB plane costs ~0.29 s per prime per
  multiply. Feed for B9: a kernel writing the result directly into the
  registered buffer would take ~5 ms.

## 21. B5 — 4-way interleaved peer gather (`bench/19_peer_gather`), 2 GiB planes

| pattern | per APU GB/s | node |
|---|---:|---:|
| local gather, 4 local planes (reference) | 2 566–2 756 | 10 799 |
| **peer gather, 64-bit loads** (3 remote + 1 local) | **153.7–153.9** | 615 |
| peer gather, 128-bit loads | 146.2 | 585 |
| `hipMemcpyPeerAsync` copy-in of 3 planes | 175 (APU2: **87.7**) | 614 |
| staged: copy-in then local gather | 213 (APU2: 112) | 752 |

**Passes** (≥ 100 GB/s). The paper's `crt4_gpu` direct peer-read pattern is
viable; 64-bit is again the right width across the fabric (bench/11). Staging
through blit copies is 1.4× faster overall but APU2's copy-in runs at half
rate — a link or engine asymmetry to keep in mind for any blit-based design.

## 22. B8 — allocation costs (`bench/21_alloc`), APU0

| GiB | `hipMalloc` | `hipFree` | `hipHostMalloc` | `hipHostRegister` | malloc + 48-thread touch | calloc + touch |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.062 s | 0.004 | 0.146 | 0.090 | 0.040 | 0.024 |
| 8 | 0.286 | 0.023 | 0.966 | 0.592 | 0.067 | 0.081 |
| 16 | 0.567 | 0.046 | 1.970 | 1.167 | 0.130 | 0.138 |
| 32 | 1.170 | 0.093 | 3.781 | 2.374 | 0.263 | 0.277 |

`hipMalloc` ≈ 37 ms/GiB, matching the paper's "0.5–1 s per multi-GB buffer";
grow-only pools are justified. `hipHostMalloc` is 3.3× dearer but arrives
pre-faulted (GPU first touch is free afterwards); `hipHostRegister` is 2×.
Single-thread CPU first touch is 64 ms/GiB, 48 threads 8 ms/GiB. **calloc costs
nothing extra on fresh allocations** — the paper's "malloc over calloc saves
≈ 5 s" must come from pool reuse, where calloc has to zero already-faulted
pages.

## 23. B7 — 300 s sustained at 256 GiB resident (`bench/14_sustained 300 64`)

64 GiB `hipHostMalloc` per APU (alloc + touch 36.6 s worst), the kernel
rotating through it in 512 MiB slices, all four APUs, `amd-smi` sampled every
2 s.

| window | node Gbfly/s | vs first |
|---:|---:|---:|
| 5 s | 2 338.6 | 1.000 |
| 110 s | 2 323.2 | 0.993 |
| 200 s | 2 319.2 | 0.992 |
| **300 s** | **2 316.4** | **0.990** |

**1.0 % decay over 5 minutes** at the full footprint; R8's conclusion holds.
But the telemetry shows *why* it is flat:

| sustained phase | value |
|---|---|
| socket power, every APU | **550 W, pinned — the package power cap** |
| sclk | mean **1 485 MHz**, range 1 395–1 516 (max is 2 100) |
| hotspot | 65 → 84 °C over the run |

**CORRECTION to campaigns 2–3.** Under this workload the APUs are power-bound
at ~1.49 GHz, not clock-bound at 2.10 GHz. Every `cyc/op` in §9–10 divided a
measured rate by 30.64 Tlane-cycles/s (228 × 64 × 2.10 GHz); if the clock
during those bursts was also ~1.49 GHz, the true cycle counts are ~0.71× the
quoted ones (butterfly 27.7 → ~19.6 cyc; Shoup modmul 15.2 → ~10.7). The
ratios and every conclusion drawn from them are unchanged; the absolute
cycle counts and the "88 % issue efficiency" claim need the clock measured
during the burst — added to PLAN.md 7.2. Any reproduction target for the
paper must likewise be read as power-limited throughput.

## 24. Burst clock (`bench/22_clock`) and the campaign-4 baseline

`clock64()` (shader cycles) over `wall_clock64()` (100 MHz) inside a
VALU-bound Shoup-butterfly kernel, so the number is the clock the kernel
itself saw. Bursts of 1 ms … 1 s, all four APUs together and APU0 alone:

| burst | sclk, all four (MHz, range) | sclk, APU0 alone | Gbfly/s per APU |
|---:|---:|---:|---:|
| 1 ms | 1 397–1 470 | 1 418 | (launch-ramp noise) |
| 3 ms | 1 463–1 565 | 1 549 | 990 |
| 10 ms | 1 440–1 487 | 1 486 | 980 |
| 30 ms | 1 516–1 535 | 1 530 | 1 002 |
| 100 ms | 1 546–1 574 | 1 578 | 1 042 |
| 300 ms | 1 564–1 594 | 1 594 | 1 053 |
| 1 000 ms | 1 569–1 596 | 1 594 | 1 052 |

**CORRECTION to campaigns 2–3 (§9, §10), now quantified.** The shader clock
under dense 64-bit VALU issue is **1.47–1.60 GHz at every burst length**, and
a *single* APU running alone gets no more than the four together — so this is
a per-APU power/current limit under this instruction mix, not the shared
550 W package cap (which bench/14 also hits at 5-minute scale). 2 100 MHz is a
light-load figure that this workload never sees. All `cyc/op` in §9–10 were
computed against 30.64 Tlane-cycles/s (2.10 GHz); at the measured ~1.55 GHz
the true cycle counts are **0.74×** those quoted:

| §9 figure | quoted cyc | at 1.55 GHz |
|---|---:|---:|
| `v_mad_u64_u32` | 1.50 | 1.1 |
| `mul_hi64` | 7.88 | 5.8 |
| `sel64` | 4.88 | 3.6 |
| Shoup modmul | 15.17 | 11.2 |
| **full butterfly** | 27.67 | **20.4** |

28 VALU instructions in ~20.4 cycles means the "88 % issue efficiency" in §2
was an under-estimate; the VALU is effectively saturated, which strengthens
R9's conclusion that instruction count is the wrong lever. Every ratio and
every design decision in §9–15 is unchanged. New rule for the suite: report
rates in Gop/s, and derive cycles only with the clock measured by
`22_clock`-style instrumentation in the same kernel.

### Baseline re-run of the whole set (`results/20260912_1727_ppac-pl1-s24-16/`)

All eleven verifying programs pass (`VERIFY`/`CHECK` OK). Node figures vs the
recorded campaigns:

| program | metric | recorded | now |
|---|---|---:|---:|
| 01 | shoup_u64 / shoup_f64 Gbfly/s | 3 886 / 4 212 | 3 880 / 4 184 |
| 02 (100 GiB/APU) | read / triad GB/s | 14 000 / 11 800 | 14 436 / 12 203 |
| 03 | push / pull GB/s | 697 / 399 | 699 / 396 |
| 06 | reg / +swizzle / ablated | 2 211 / 2 325 / 2 817 | 2 290 / 2 274 / 2 944 |
| 07 | separate / +ulong2 / +NT | — / +6 % / 0.66× | 2 195 / 2 080 / 1 364 |
| 11 | push 64 / 128 / 256-bit | 909 / 699 / 368 | 906 / 699 / 368 |
| 12 | simple / register inverse | 1 522 / 2 569 | 1 540 / 2 545 |
| 13 | ms per multiply, 1 APU | 1.23 | 1.08 |
| 14 | baseline / sustained 60 s | 2 225 / 2 327 | 2 268 / 2 328 |

Two deviations worth a note: the XOR swizzle's +5 % (§3) and the interleaved
ulong2 twiddle's +6 % (§3) did **not** reproduce on this run (−0.7 % and
−5 %). Both are small effects at the level of run-to-run variance seen in the
1–3 ms bursts above; they need repeated measurements (and 22_clock-style
clock readings) before either is relied on. Everything else is within 3 %.

## 25. B2 — the paper's tiled DIF NTT (`bench/16_ntt_tile`)

A faithful-in-structure implementation: one long DIF transform in passes of
up to 7 stages, `TILE = 128` tiles spaced 2^s_lo apart, 16 tiles per block so
every global access is a 128 B row, LDS `sh[128][17]` (17 416 B, 3 blocks/CU),
thread (tt, bb) owns column bb, `b1` kernel for stages 0–9, the paper's FP64
Barrett modmul, scale/canon fused into the last pass. Verified against a host
DIF at 2^20 for pass splits of ≤ 7, 5, 6, 4 and 3 stages and for 64 batched
2^14 transforms — all bit-identical.

Two things had to differ from the paper's description to make it correct
and fast, and both are rules for Phase 3:

1. **Butterfly adds and subtracts must be integer.** u + v with u, v < 2p ≈
   2^52.8 does not fit a double; the first version was wrong by a few units
   in every output. FP64 is for the product only.
2. **Per-thread twiddle exponentiation, as described, halves the speed.**
   T_H = w_n^(c·n/(TILE·h_min)) by square-and-multiply is ~62 modmuls per
   thread per pass against ~56 for the butterflies. A two-level 2 × 4096
   table (one modmul) took the 2^31 transform from 302 ms → 155 ms (with the
   3-blocks/CU LDS fix) → **136 ms**.

| n | passes | ms (best of 4 APUs) | effective TB/s (16 B × n × passes) |
|---:|---|---:|---:|
| 2^28 | 3 b16 + b1 | 33 | 1.0 |
| 2^30 | 3 b16 + b1 | 120 | 1.1 |
| **2^31** | [24..30] [17..23] [10..16] [0..9] | **134–138** | **1.0** — paper 1.08 |

Per pass at 2^31: b16 1.0 / 0.8 / 1.1 TB/s, b1 1.2 TB/s. Batched (2^28
points per APU as B sub-transforms), "pass-traffic" GB/s per APU:

| log L | here | paper |
|---:|---:|---:|
| 11 | 343 | 103 |
| 14 | 920 | 1 146 |
| 17 | **1 150** | ~1 300 |
| 20 | 810 | (saturates) |

**Q3 resolved: the paper's NTT is not HBM-bound.** 1 TB/s is 27 % of the
3.7 TB/s the same kernel shape reads at (§20); the passes are bound by LDS
traffic and the FP64 modmul (7 modmuls per element per pass with the
twiddle combine). "Effective bandwidth" is the paper's metric, not its
limiter. Register-blocking (bench/06's structure) and a Shoup engine are the
levers, which is Phase 5 items 1 and 3.

**Gate passes** (−7 % on the 2^31 transform, −12 % on batched log L = 17).

## 26. B4 — kernels on pinned host staging (`bench/18_staging`), NUMA-local

| pattern, per APU | GB/s |
|---|---:|
| `scatter_expand_k`: M × L_sub = 2^28 limbs read from host at prefix offsets, zero-extended into 2 L_sub planes, L_sub = 2^10 / 2^14 / 2^17 / 2^20 | 610–634 / 760–786 / 738–789 / 741–792 |
| contiguous host → device kernel copy | 1 730–1 815 |
| **result store, device → host by kernel** | **1 773–1 852** |
| result store, device → host by `hipMemcpy` (§20) | 58.5 |

Reading operands straight from pinned host is fine (the paper's "no
intermediate device hop" costs nothing), and a kernel storing results into
the staging buffer is **31× faster than the D2H memcpy the paper uses**.

## 27. B6 — the CPU side (`bench/20_cpu`), 192 threads on 96 Zen4 cores

`crt_carry_par4` as the paper describes: Garner with M₁ (128-bit) and M₂
(192-bit) precomputed, `__int128 %` for the modular steps, T stripes each
accumulating a 4-limb coefficient at limb offset k with an 8-limb spill,
sequential spill merge. Verified against GMP at 2^16 coefficients.

| | value |
|---|---:|
| CRT + carry, 2^30 coefficients, 48 / 96 / 192 stripes | 1.49 / 0.82 / **0.63 s** (1.69 Gcoef/s) |
| → 2^31 coefficients (one 4-prime plane set) | **≈ 1.3–1.6 s** — passes (≤ 3 s) |
| same, while all four APUs run a VALU kernel | −11 % (noise): **no interference** |
| repack (OpenMP copy) 2^30 limbs | 0.052 s = 328 GB/s |
| STREAM triad, 2^30 doubles | 194 GB/s |
| seed spans, 512 terms, GMP, all 5.98 × 10^6 spans at d = 4 × 10^10 | 43–77 s (run-to-run) |

The seed estimate is a sequential 512-term merge per span; a product tree
inside the span would be several times faster. The paper's whole bs phase is
112 s, so its seeding must already be smarter than that.

## 28. B9 — component model of one mdev multiply at 2^31 points, 4 primes

From §19–27, all four APUs in parallel, one prime each:

| step | cost | source |
|---|---:|---|
| repack A, B to hstage (CPU) | 0.10 s | §27 328 GB/s, 2 × 16.6 GB |
| copy + canonicalise A, B host → device | 0.02 s | §26 1.7 TB/s |
| forward NTT × 2 | 0.27 s | §25 136 ms |
| pointwise (fused) + inverse NTT | 0.14 s | §25 |
| D2H `hipMemcpy` of the 16 GiB plane | 0.29 s (0.065 s with 8 streams, §30 D7) | §20 58.5 GB/s |
| CPU CRT + carry, 2^31 coefficients | 1.4 s (0.6 s with the tight carry window, §34) | §27 |
| **total** | **≈ 2.2 s** (≈ 2.0 s with the 8-stream blit) | |

Against the paper's 10dP = 12.6 s: that phase is T = 10^d by repeated
squaring (≈ 2 full-size multiply-equivalents summed over the doubling sizes)
plus A = T·P, which at 4.16 × 10^9 limbs needs a 2^32 transform (≈ 2
equivalents, and Q1 still stands) — ≈ 4 × 2.2–2.5 s ≈ 9–10 s plus
allocation and pool growth. **The model reproduces the paper's phase time
within its uncertainty, and says where it goes: 77 % of an mdev multiply is
the CPU CRT and the D2H blit, only 19 % is the NTT.** The GPU S-stripe CRT
through peer gather (§21: 64 GiB gathered at 4 × 154 GB/s ≈ 0.1 s) and a
kernel store to host (§26) would take the multiply to ≈ 0.6 s — Phase 5.

**Phase 1 complete.** Every gate passed; the paper is reproducible on this
node, with the caveats in §19 (lazy × lazy modmul), §20 (NUMA pinning),
§25 (integer adds, table twiddles) and Q1 (transform length for 10dP).

## 29. Phase 1b — node partition survey (`nodecheck.sh`, `results/0_node_*.txt`, `results/1b_*.txt`)

Question: aac6 nodes differ in memory segmentation — could that account for
any discrepancy with the paper? Reference: ROCm blog "MI300 compute and
memory partition modes" (SPX/CPX × NPS1/NPS4; NPS4 only with CPX; NPS4 gives
5–10 % more STREAM bandwidth and higher clocks on MI300X).

| node(s) | APUs | compute | memory | NUMA | HBM per device | power cap |
|---|---:|---|---|---|---:|---:|
| `ppac-pl1-s24-{16,26,30}` | 4 | **SPX** | **NPS1** | 4 × 128 GB, dist 10/32 | 128 GiB | 550 W |
| `sh5-pl1-s12-33` | 1 | SPX | NPS1 | 1 | 128 GiB | 550 W |
| `sh5-pl1-s12-{09,12,15,36}` | 1 | **CPX** (6 devices) | NPS1 | 1 | 21.3 GiB each | 550 W |
| `ppac-pl1-s25-40` (down) | 4 | CPX (24 devices) | ? | ? | ? | ? |

All three 4-APU SPX nodes are identical (same BIOS `RMP1001AS`, same
kernel, same limits). **No node on aac6 is in NPS4.** The paper's platform
("4×MI300A, unified coherent HBM ≥ 500 GB", four devices) is SPX/NPS1 — the
same as every number in §17–28. Partition mode therefore explains none of
the Phase 1 discrepancies.

What the modes do change, measured with `22_clock` and `02_capacity`:

| | sclk under VALU load | Gbfly/s | HBM read |
|---|---:|---:|---:|
| PPAC SPX, one APU of four (§24) | 1 594 MHz | 1 052 | 3.7 TB/s |
| SH5 SPX, single APU | 1 584 | 1 036 | 3.82 TB/s |
| SH5 CPX, all 6 partitions busy | 1 547–1 570 | 1 037 total | 3.64–3.89 TB/s total (605–650 each) |
| **SH5 CPX, one partition alone (38 CU)** | **1 956 MHz** | 216 | — |

**D6 verdict: (D), genuine and now explained.** A sixth of the APU runs at
1 956 MHz; the whole APU under the same instruction mix runs at ~1 580. The
limit is the APU's power/current budget for dense 64-bit VALU issue, and it
is the same on every node and in both partition modes. It also confirms the
`clock64()`/`wall_clock64()` reading tracks sclk (the solo-partition value
lands near the 2 100 nominal), so (E) is excluded. Aggregate throughput and
HBM bandwidth are identical in SPX and CPX — CPX buys nothing for this
workload on MI300A, unlike the MI300X GEMM case in the blog.

Consequence for node choice: any of the three PPAC SPX nodes; results are
interchangeable. `-w` still pins one per campaign for cleanliness.

**Archived (2026-09-12):** `ppac-pl1-s25-40`, the only 4-APU CPX node (24
devices), has been down since 2026-08-04. If it ever returns, run `nodecheck.sh`, `22_clock`,
`02_capacity 4` and `17_hostreg 4` there — it is the one configuration on
aac6 that could differ from the above in NUMA layout or clock behaviour
(PLAN.md D6 follow-up).

## 30. Phase 1b — the short items (`results/1b_20260912/`)

| item | verdict | evidence | consequence |
|---|---|---|---|
| **D2** one-correction failure 0.57 % vs 0.21–0.37 % | **(A) partly; the paper's number is not reproducible with uniform inputs** | [0, 4p) × canonical (an *unfolded* u − v + 2p): two corrections wrong **21 %**, one correction wrong 23.7 % — so the paper's kernel must fold to [0, 2p) before the multiply, and its 0.57 % comes from a non-uniform operand distribution we cannot know. The engineering facts stand: fold first; two corrections are exact for [0, 2p) × [0, p) | Phase 3 rule 1 unchanged, plus: fold *before* every multiply, never after |
| **D3** FP64 lazy adds inexact | **(A)** paper omits that adds are integer | §25 | Phase 3 rule 2 |
| **D7** D2H 58.5 GB/s flat | **(E) our measurement — single stream.** With 8 chunks in flight on created streams: **262 GB/s per APU** into `hipHostMalloc` (4.5×). Into registered memory the same only when the buffer is NUMA-local (APU3: 252; the others, whose buffers I left on one node: 62) — and single-stream is 58 GB/s local or remote, so the earlier flat figure was a stream limit, not a placement effect | corrects §20/§26: blit ceiling ≈ 260 GB/s per APU, still **7× below a kernel store** (1 800). B9: D2H term 0.29 → 0.065 s; multiply ≈ 2.0 s |
| **D9** NUMA interleave 26–55× vs "+20 s" | **(A)** | harmonic mean of ¼ local / ¾ remote predicts 89 GB/s; measured 144 (the three remote quarters use three links in parallel). Consistent with the paper's +20 s on the staging fraction of its traffic | Phase 3 rule 4 |
| **D10** calloc "≈ 5 s" | **(A), unverifiable with glibc**: 16 GiB three rounds, malloc 0.14–0.19 s vs calloc 0.14–0.17 — no penalty even with `M_TRIM` off, because glibc caps `M_MMAP_THRESHOLD` at 32 MiB and always mmaps this size (fresh zero pages). The paper's saving can only come from its own pool zeroing on reuse | none; use malloc in pools and never memset what is fully overwritten |
| **D11** log L = 11 batched 343 vs 103 GB/s | **(A)** our kernel does the work: 512 × 2^11 batched verified bit-exact vs host DIF. The paper's 103 must include the batch tier's expand/CRT or use another convention | none |
| **D13** XOR swizzle +5 %, ulong2 +6 % (campaign 1) | **(E) noise-level.** Five runs: swizzle +0 … +6 % (mean +2 %); ulong2 −6 … +4 % (mean 0). Run-to-run spread ±3 % node, per-APU spread up to 20 % within a run (480–600 Gbfly/s) | keep the swizzle (never negative, and it was derived from the bank model); drop the ulong2 claim. Per-APU spread is the larger open question — likely per-APU power/clock (§24, §29) |
| **D1** modmul 1.72× (desk part) | open | in-kernel rate from §25: 7 modmuls × 2³¹ in 34 ms = 442 Gmodmul/s per pass — neither the chain (1 330) nor the paper (775). "12.4 TF64 / 16 ops" cannot be reconciled without an instruction count | needs `make isa` (Phase 2) |

Still open in Phase 1b: D1 (ISA count), D4 (block-base twiddle variant), D5
(STG templates, larger b1), D8 (CRT profile and Barrett), D12 (Q1, deferred),
and the `ppac-pl1-s25-40` follow-up to D6.

## 31. Phase 1b — D1: the modmul rate (`make isa`, `bench/15_barrett_f64` D1 variants)

`make isa B=15_barrett_f64 K=k_rate` (new: `isa.py` counts instruction
classes per kernel and in its innermost loop). The compiled chain loop holds
8 modmuls; per modmul:

| class | count | note |
|---|---:|---|
| `v_fma_f64` | 2 | Dekker low part; r = hi − q·p |
| `v_mul_f64` | 2 | hi = a·b; hi·pinv |
| `v_floor_f64` (`v_rndne` for the rint variant) | 1 | q |
| `v_add_f64` | 5 | + lo, four corrections |
| `v_cmp_*_f64` | 4 | four corrections |
| `v_cndmask_b32` | 8 | four 64-bit selects |
| **VALU total** | **22** | 14 FP64 ops + 8 selects; the paper says "16 FP64 ops" |

(E) excluded: nothing is simplified away — all four corrections are present,
`rint` compiles to the identical count, three corrections add 8.

In-kernel clock (`clock64()/wall_clock64()` in the chain kernels): **1 590 MHz**
during the FP64 chain, 1 630 during the Shoup chain. So 1 330 Gmodmul/s × 22
= 29.3 T VALU instr/s per APU against 228 × 64 × 1.59 GHz = 23.2 T lane-cycles:
**1.26 instructions per lane-cycle** — the 32-bit `cndmask` selects co-issue
with the FP64 pipeline. (This also revises §24's "VALU saturated" reading:
the FP64/int64 pipe is saturated; there is spare issue for cheap 32-bit ops.)

Every configuration that could have produced the paper's 775 was tried, all
on P[1], per APU:

| variant | Gmodmul/s |
|---|---:|
| as measured (8 chains, 8 blocks/CU, floor, 2 corrections) | 1 331 |
| `rint` quotient | 1 328 |
| three corrections per direction | 980 |
| 4 / 2 / **1** independent chain(s) | 1 320 / 1 295 / **1 252** |
| 3 blocks/CU (the paper's NTT occupancy) | 1 255 |
| 1 block/CU | 895 |

Even a single dependent chain at 1 block/CU would sit near 850; nothing
reaches down to 775 except adding work the paper does not describe.

**D1 verdict: (D) — genuine.** The hardware executes the paper's modmul, as
written, at 1.6–1.7× the paper's stated rate under every ILP, occupancy and
rounding variant; the clock is the same 1.59 GHz as for the integer mix; the
instruction count (22, 14 FP64) is at or below the paper's "16 FP64 ops". The
most likely reading is that "775 Gmodmul/s at 12.4 TF64" is an *effective*
figure derived from their whole NTT kernel's FLOP throughput (our in-kernel
equivalent is 442 G/s per pass, §25 — also not 775), or was measured on a
different ROCm/firmware. It does not affect reproduction: the reproduction is
faster, not slower, at this stage.

## 32. Phase 1b — D4: twiddle base strategy (`bench/16_ntt_tile`, `g_twmode`)

Three ways to get each thread's T_H = w_n^(c·K) at the start of a b16 pass,
all verified bit-identical against the host DIF at 2^20, timed on the full
2^31 transform (3 b16 passes + b1), best of 3, per APU:

| mode | per thread | 2^31 transform |
|---|---|---:|
| 0 two-level table (`tlo[c & 4095] · thi[c >> 12]`) | 1 modmul + 2 loads | **135–140 ms** |
| 1 per-thread exponentiation (the paper's words, literally) | ~62 modmuls | 154–157 ms (**+13 %**) |
| 2 one exponentiation per block (thread 0, LDS broadcast) · `tab[bb]` | 1 modmul + serial dpow latency per block | 148–152 ms (+9 %) |

**D4 verdict: (A) with a caveat.** "Computed per thread" is consistent with a
scheme that costs one modmul per thread *if* the per-block base comes from
somewhere cheap; a literal per-thread exponentiation costs 13 % on the whole
transform (not the 2× first seen in §25 — that figure was confounded with the
LDS-occupancy fix), and computing the base once per block still costs 9 %
because the serial square-and-multiply sits on the block's critical path
before the first stage. The table is the right implementation; the paper's
description is not wrong, just under-specified. Phase 3 rule 3 stands.

## 33. Phase 1b — D5: the NTT bandwidth gap (`bench/16_ntt_tile`, `g_tmpl`, `g_lgl`)

Two changes to the b16 kernel, both as the paper's "template<STG>" implies:
STG is a compile-time parameter (every stage loop unrolled, shifts constant),
and a block always holds 128 rows × 16 columns = 2048 elements — for
TILE < 128 it takes 128/TILE consecutive slabs of TILE rows, so a 4-stage
pass no longer runs 256-element blocks. Each thread keeps 1/2/4 twiddle bases
in registers for the tiles its four butterflies touch. Plus `b1` as a
template on the block length 2^lgl for lgl = 10, 11, 12. All variants
verified bit-identical against the host DIF at 2^20 (seven splits).

| | runtime kernel (§25) | **template, lgl = 10** | template, lgl = 11 | template, lgl = 12 | paper |
|---|---:|---:|---:|---:|---:|
| 2^31 transform, ms | 135–139 | **117–118** | 126–129 | 153–155 | — |
| 2^31 effective TB/s | 1.0 | **1.17** | 1.1 | 0.9 | 1.08 |
| batched log L = 14, GB/s per APU | 870–910 | **1 444–1 464** | 1 190 | 825–842 | 1 146 |
| batched log L = 17 | 1 118–1 158 | **1 361–1 396** | 1 093–1 109 | 742–753 | ~1 300 |
| batched log L = 20 | 470–487 | **1 494–1 554** | 1 321–1 333 | 977–984 | (saturates) |

A longer `b1` loses: 2^11 needs 24 KiB of LDS (2 blocks/CU), 2^12 needs 48 KiB
(1 block/CU), and occupancy costs more than the extra in-LDS stages save.

**D5 verdict: (E) — our kernel, not the hardware or the paper.** With the
stage count compiled in and full blocks for short passes, the reproduction is
**8 % faster than the paper on the 2^31 transform, 27 % faster on batched
log L = 14 and 6 % on log L = 17**, all within or above the pass criterion.
The earlier −7 % / −20 % gaps were the runtime stage loop and the
256-element blocks of 4-stage passes. Phase 3 uses the template kernels;
the byte-convention question (A) is moot since both figures now clear the
paper's.

## 34. Phase 1b — D8: where the CPU CRT time goes (`bench/20_cpu`, D8 modes)

2^30 coefficients, 192 stripes, best of 3 (run-to-run ±10 %):

| variant | s | GB/s of residue + result traffic |
|---|---:|---:|
| memory only (read 4 residues, write 1 limb) — the floor | 0.215–0.249 | 172–200 (= STREAM, §27) |
| Garner only (`__int128 %`), no carry | 0.301–0.379 | 113–143 |
| **as measured in §27: Garner + 8-limb sliding carry window** | 0.531–0.594 | 72–81 |
| Garner with FP64-Barrett reductions instead of `__int128 %` | 0.996–1.012 | 42 |
| **Garner + tight 3-limb window + carry bit** | **0.288** | **149** |

All variants GMP-verified at 2^16 coefficients.

- The Garner reductions are **16–22 %** of the original time; Zen4's `divq`
  is not the bottleneck, and replacing it with FP64 Barrett is **1.7–1.9×
  slower** (conversions and corrections cost more than the divide). The
  hypothesis behind D8 test (2) is rejected.
- The **carry loop was the cost**: an 8-limb `__int128` sliding window per
  coefficient. Since a coefficient is < 2^207, a 3-limb pending window plus a
  carry bit is exact, and it runs at **2.06× the original — within 16 % of the
  memory floor**.

**D8 verdict: (E) — our implementation.** Per 2^31 coefficients the CPU CRT
is ≈ 0.6 s, not 1.4 s. Revised B9 model of one 2^31-point 4-prime mdev
multiply: repack 0.10 + H2D 0.02 + 2 forward NTT 0.23 (§33) + inverse 0.12 +
D2H 0.065 (§30) + CRT 0.6 = **≈ 1.1 s** (was 2.2). CRT + D2H are now ~60 %
of the multiply rather than 77 %; the GPU peer-gather CRT of §28 would gain
≈ 2×, not 4×. 10dP at ≈ 4 multiply-equivalents is then ≈ 5 s against the
paper's 12.6 s — consistent with the paper having the slower carry, or
counting pool growth and T = 10^d in the phase.

## 35. Phase 1b — D12 / Q1: how does 10^d·P fit a 2^31-point pool? (desk)

Facts. At d = 4 × 10^10: P, Q, T = 10^d are each ≈ 2.08 × 10^9 limbs
(0.97 × 2^31). With b = 64 (one limb per point) a product of two such numbers
has 4.16 × 10^9 limbs and needs a 2^32-point transform: 32 GiB per residue
plane, 64 GiB per device for da + db, **256 GiB of device pools** — twice the
paper's stated `ch_da/ch_db` = 128 GB (pregrown 2^31). The paper's own tier
table has MSL = 2.1 G: "2·max_nl ≤ MSL → mdev-serial", i.e. a single mdev
multiply takes operands up to 2^30 limbs, products up to 2^31. Larger
operands go to "mdev-parallel (one device per worker)", which does not split
a product either. Nor is 10dP the only such product: Newton's last iteration
(Q × r at full precision) and `mul_hi_shift(A, μ)` in the division are the
same size or larger.

Candidates, costed with the §34 model (one 2^31-point 4-prime multiply ≈
1.1 s) and the paper's memory table:

| candidate | device pools | multiply-equivalents for 10dP | model time | fits paper's 128 GB / 256 GB RSS? | fits this node (502 GB)? |
|---|---:|---:|---:|---|---|
| (a) grow pools to 2^32 for the phase | 256 GiB | 1 (A) + ~2 (T by squaring) | ≈ 3.3 s | **no** — contradicts 128 GB | yes: 256 GiB device + ~130 GB host |
| (b) split both operands in halves, 4 products of 2^31 | 128 GiB | 4 + ~2 | ≈ 6.6 s | yes | yes |
| (b′) Karatsuba at the top, 3 products | 128 GiB | 3 + ~2 | ≈ 5.5 s | yes | yes |
| (c) fold 10^d into the reciprocal (X = ⌊P · 10^d μ⌋) | 128 GiB | still a 2^32 product (P × 10^d μ) | — | no better than (a)/(b) | — |
| (d) A = (5^d · P) << d | 128 GiB | 5^d·P is 3.5 × 10^9 limbs: still > 2^31 | — | no | — |

Only (b)/(b′) are consistent with every number the paper gives: 128 GB of
device pools, MSL = 2^31, and a 12.6 s phase (≈ 6 equivalents at the paper's
slower CRT and D2H ≈ 2.2 s each ≈ 13 s). The paper does not describe the
split anywhere.

**D12 verdict: (D) — the paper omits a required top-level splitting step**
for every product larger than 2^31 points (10dP, the last Newton
iteration, the division's `mul_hi_shift`), and its memory table is only
consistent with such a split. This is not a reproduction blocker: it is one
routine (`rns_multiply_split`: halve both operands, 4 mdev products or
Karatsuba 3, add with carry) on top of the mdev tier.

**Decision needed for Phase 3 (the deferred memory discussion):** either
(i) reproduce faithfully — pools at 2^31, add the split (b′), ≈ 5.5 s for
10dP; or (ii) exploit the node — grow pools to 2^32 (256 GiB device, ≈ 390 GB
total with host buffers, within the 502 GB), no split, ≈ 3.3 s. (ii) is
faster and simpler but is not what the paper ran and leaves ~110 GB for
everything else at the peak, which is tight against the ≤ 50 + ≤ 33 + ≤ 35 GB
the paper lists for its phase-scoped buffers. Recommendation: build (i) —
the split is needed anyway for any digit count above the pool size, which
is exactly the lever for going past 4 × 10^10 digits later — and keep (ii)
as a switch.

## 36. Campaign 4 summary — the paper against this node, after Phase 1b

| quantity | paper | measured here | section |
|---|---:|---:|---|
| FP64 Barrett modmul, per APU | 775 Gmodmul/s | 1 330 (22 VALU instr, 1.59 GHz) | §19, §31 |
| corrections needed | 2 | 2, and only for ≤ one lazy operand in [0,2p); fold before every multiply | §19, §30 |
| 2^31-point DIF transform | 1.08 TB/s effective | **1.17 TB/s** (117 ms) | §25, §33 |
| batched NTT, log L = 14 / 17 | 1 146 / ~1 300 GB/s | 1 450 / 1 380 | §33 |
| `hipHostRegister` 64 GB | works | works (4.8 s); HBM-speed if NUMA-local, 55× slower if not | §20 |
| D2H return | (memcpy) | 58 GB/s single stream, 262 with 8 streams, 1 800 by kernel store | §20, §26, §30 |
| GPU CRT peer gather | (S-stripe) | 154 GB/s per APU | §21 |
| CPU CRT + carry, 2^31 | (in 12.6 s phase) | 0.6 s with a tight carry window | §27, §34 |
| one 2^31-point 4-prime multiply | ~2–3 s (implied) | ≈ 1.1 s modelled | §28, §34 |
| 10dP phase | 12.6 s | ≈ 5.5 s with a Karatsuba top split (the paper's unstated step) | §35 |
| sustained throughput | (286 s run) | −1 % over 300 s at 256 GiB; 550 W cap, ~1.5 GHz | §23, §24, §29 |
| partition mode | 4 devices, unified HBM | SPX / NPS1 on every 4-APU node; no NPS4 on aac6 | §29 |

Nothing measured prevents the reproduction. §35's decision was taken on
2026-09-12: (i) 2^31 pools with a Karatsuba top-level split by default,
(ii) 2^32 pools kept as a switch in case a more detailed follow-up paper
describes the actual method. The `ppac-pl1-s25-40` measurement is archived
(node down since 2026-08-04); it is opportunistic, not a dependency.

## 37. Phase 2 close-out (`results/1b_20260912/{09_p2,infcache,mfma}.log`)

### 09_logic, restated with opaque chains, the measured clock, and true latency
`asm volatile` barriers stop the compiler folding dependent chains; cycles are
now divided by the clock measured in-kernel (**1 481 MHz** during this run);
latency is one wave per CU with nothing to overlap, so it is absolute.

| op | Gop/s | cyc/op | latency, cyc | §9 quoted cyc (at 2.10 GHz) |
|---|---:|---:|---:|---:|
| `v_mad_u64_u32` | 21 895 | **0.99** | 37 | 1.50 |
| `mul_lo64` / `mul_hi64` | 7 046 / 4 309 | 3.07 / 5.02 | 46 / 63 | 4.27 / 7.88 |
| `add64` / `sub64` | 23 655 / 12 478 | 0.91 / 1.73 | **26 / 54** (now valid) | 1.41 / 2.53 |
| `sel64` | 6 278 | 3.44 | 51 | 4.88 |
| Shoup modmul | 1 895 | **11.4** | 111 | 15.17 |
| Montgomery | 1 204 | 18.0 | 149 | 25.46 |
| **full butterfly** | 1 065 | **20.3** | 146 | 27.67 |
| `fma64` / `dmul64` / `rint64` | 18 041 / 19 763 / 11 253 | 1.20 / 1.09 / 1.92 | 26 / 26 / 37 | 1.38 / 1.39 / 3.02 |
| `add32` / `xor32` / `mul_lo32` | 29 269 / 29 312 / 25 985 | 0.74 / 0.74 / 0.83 | 26 / 26 / 32 | — |

The ×0.74 restatement in §24 was right (butterfly 27.67 → 20.3). Two things
are new: `v_mad_u64_u32` and the 64-bit add are **one lane-cycle** each, and
the 32-bit ops go below one cycle (0.74) — the co-issue seen in §31. The
dependent-op latency is 26 cycles for the simplest ops and 146 for a whole
butterfly, so ILP of ~7 chains saturates the butterfly (the register-blocked
kernels hold 8 points per thread — consistent).

### mem/infcache — the operand split's gather pattern vs working set
One 8-byte load per lane at a random limb, 2^28 accesses per APU, all four:

| working set | useful GB/s per APU | line traffic | chase latency |
|---:|---:|---:|---:|
| 8 MB | 534 | 4.3 TB/s | 300 ns (630 cyc) |
| 32 MB | 306 | 2.5 | 300 ns |
| 128 MB | 276 | 2.2 | 470–515 ns |
| 256 MB | 272 | 2.2 | 505–585 ns |
| 512 MB | 269 | 2.2 | 620–640 ns |
| 2 GB | 215–237 | 1.8 | 635–660 ns |
| 8 GB | 198–227 | 1.7 | 645–665 ns |

Infinity Cache latency ≈ 300 ns, HBM ≈ 650 ns; but for *throughput* the
random-line gather is request-rate bound at ~2.2 TB/s of lines from the
Infinity Cache and ~1.8 TB/s from HBM. **§14's "cheap because the packed
operand fits in the Infinity Cache" is only a 1.35× effect** (306 vs 226 GB/s
useful for a 25 MB operand): the split is cheap because a gather of one line
per lane still moves ~2 TB/s of lines, not because of the cache. Working
sets above 2 GB show a per-APU spread (APU3 slower) that matches its lower
clock elsewhere.

### arith/mfma — matrix cores, for the record (R10 source)
| | per APU | node | datasheet at 2.1 GHz | scaled to measured clock |
|---|---:|---:|---:|---:|
| int8 32×32×16, 4 chains | 601–724 TMAC/s at 1 272–1 427 MHz | 2 739 | 980 | 663 at 1.42 GHz — matches |
| bf16 32×32×8 | 356–421 | 1 542 | 490 | — |
| f64 16×16×4 | 21.2–21.7 (43 TF) | 86 | 30.7 | 21.6 at 1.48 GHz — matches exactly |

R10's 880 TMAC/s int8 must have been a shorter burst at a higher clock; the
matrix cores are power-limited like the VALU (int8 pulls the clock to
1.27–1.43 GHz). The R10 conclusion (matrix cores are ≤ 7× the VALU on
modular products, not enough to beat the butterfly network) is unchanged:
at the measured clocks the ratio is 700 T / (1.9 G × 64) ≈ 5.8×.

### Housekeeping
04, 05, 08 moved to `bench/old/` (`make old`); `bench/{arith,mem,lds,fabric,
kernel,sustained,system}/` created with the first two programs above; `./suite`
default set now covers 01–23 plus `mem/infcache` and `arith/mfma` (log names
flatten the slash). D13's five-run repeat stands as the re-measurement of the
swizzle and ulong2 effects (§30). **Phase 2 complete.**

## 38. Phase 3 — steps 0–4: reference digits, arithmetic, NTT, multiply tiers, CRT (`ecalc/`, 2026-09-13)

Everything under `~/ntt/ecalc/` (`make` in that directory; `./run <test>` runs
one program on the allocated node with all four GPUs; `results/<test>.txt`).
Every module has a GMP-checked test; all of them pass on `ppac-pl1-s24-16`.

### Step 0 — `ref/gen_e` and `tests/harness.h`
GMP binary splitting with the paper's term count N = min{m : lgamma(m+1)/ln 10 ≥ d + 50},
top five tree levels as OpenMP tasks, 32 threads:

| digits | N terms | bs | division | `mpz_get_str` | hash | total |
|---:|---:|---:|---:|---:|---:|---:|
| 10⁶ | 205 032 | 0.05 s | 0.06 | 0.07 | 0.03 | 0.21 s |
| 10⁷ | 1.72 × 10⁶ | 0.41 | 0.60 | 1.15 | 0.17 | 2.3 s |
| 10⁸ | 1.48 × 10⁷ | 4.6 | 8.0 | 18.8 | 1.7 | 33 s |
| **10⁹** | 1.30 × 10⁸ | 54 | 95 | **277** | 16 | **442 s** |

`ref/e_<d>.txt` ("2." + d digits) and `ref/e_<d>.sha256` (one SHA-256 per
10⁶-digit block plus one for the whole string). The 111 blocks shared between
the four files agree, although each run used a different N. Tier-2 windows
(50 digits from the o-th fractional digit): o = 50 `59574966967627724076630353547594571382178525166427`,
10⁶ `88374711515623968271347126772832291250652542450798`, 10⁸ `25522594276661070064277361046962720701530988137395`.
GMP's radix conversion alone takes 277 s for 10⁹ digits — the yardstick for
the paper's dc phase (110 s for 4 × 10¹⁰ digits on the GPU, i.e. ~100× faster
per digit).

### Step 1 — `modarith.h`, `t_modarith` (125 checks, 10⁹ pairs per case)
* Roots: `ec_W33` matches GMP's g^((p−1)/2³³); `ec_root(i, logn)` has exact
  order 2^logn for logn ∈ {1, 2, 10, 20, 31, 32, 33}; inverses check.
* Two-correction modmul exact for canonical × canonical and lazy [0,2p) ×
  canonical on all four primes; edge pairs (0, 1, 2, p−2 … 2p−1, 2⁵¹, 2⁵²−1)
  exact; host and device versions agree on 10⁷ pairs per prime (the host copy
  builds the twiddle tables).
* One-correction failure rate for lazy × canonical: **P0 0.368 %, P1 0.214 %,
  P2 0, P3 0** (paper: 0.57 % for P[1]; D2 stands).
* **New:** lazy × lazy is *exact* on P2 and P3 (< 2^51.4) and inexact on P0,
  P1 (> 2^51.7; 0.37 % / 0.21 % of pairs). Rule 1 stays binding for every
  plane — the kernels are prime-agnostic — but a per-prime kernel could drop
  the folds on two of the four planes (Phase 5 note).
* `canon64` never needs even its first correction on 10⁹ random limbs
  (q = ⌊x μ / 2¹¹⁵⌋ is exact for x < 2⁶⁴, p > 2^51.2); Shoup lazy result
  always < 2p.
* Library modmul rate 1 331–1 355 Gmodmul/s per APU (bench/15: 1 330).

### Step 2 — `ntt.h/.c`, `t_ntt` (452 checks)
Forward DIF (natural → bit-reversed, canonical), inverse DIT (bit-reversed,
lazy in → natural, canonical, × n⁻¹ fused into the last store), pointwise,
`PW_FUSE` (pointwise folded into the first inverse pass for logn ≥ 14), a
broadcast pointwise for grpB, and `ntt_load` (canon64 + zero-extend from
host or device memory). Per-(prime, logn) twiddle sets built lazily and
cached. logn 10 … 33; `NTT_B16_STG` 3 … 7; passes of 1–2 stages exist for
the remainders (bench/16 had them; the first library version did not and
aborted at logn = 11).

| check | result |
|---|---|
| fwd vs O(n²) DFT (128-bit), n = 2¹⁰, 2¹¹, 2¹³, all primes, bit-reversed order | exact; host reference too |
| inv(fwd(x)) = x, logn 10 … 31, all primes; five generators up to 2²⁴; device inv vs host inv to 2²⁰ | exact |
| batched 64 × 2¹⁴ round trip | exact |
| cyclic convolution mod p vs schoolbook at 2¹² | exact |
| integer product, 16-bit limbs, vs `mpz_mul`, logn 16 … 20, fused and unfused pointwise | exact |
| STG 3, 4, 5, 6, 7 at 2²⁴ (6, 5, 4, 4, 3 passes) | bit-identical forward output, exact round trip |
| **2³¹ forward, per APU** | **113–116 ms = 1.19–1.21 TB/s** (paper 1.08, bench/16 1.17) |
| 2³¹ inverse with the n⁻¹ fusion | 117–120 ms = 1.14–1.17 TB/s |
| batched log L = 14 / 17 (2³¹ points) | 1 548–1 581 / 1 453–1 481 GB/s |

STG = 8 (256-row tile) is not built: 34 KB of LDS per block would allow one
block per CU (§33).

### Step 3 — `mem`, `bigint`, `rns_mul`, `t_mul` (95 checks + batch cases)
Faithful to §4.1: per-device pinned staging (malloc + node-pinned first
touch + `hipHostRegister`; 16 GiB each, 0.2–0.35 s touch, 1.2 s register),
device pools `ch_da`/`ch_db` pregrown to 2^POOL_LOG points (`rns_init(31)`;
32 is the switch for Q1 alternative (ii)), repack of A and B into every
device's staging by that node's 48 threads, `ntt_load` + fwd × 2 +
`inv_pw`, kernel store of the residue plane (rule 5), CPU CRT.

| tier | test | result |
|---|---|---|
| schoolbook (< 1 024 points), mdev, Karatsuba over halves, two-piece split, chunked 24:1 and 1:24, two-level (10× pool) | pool 2²⁰, 13 size cases × 5 generators, ×0, ×1 | exact vs `mpz_mul` |
| pool 2³¹: 2¹⁶, 2¹⁷±1, 2¹⁸±1, 2²⁰, 2²⁴, 2²⁶ limbs | vs `mpz_mul` | exact |
| 2²⁷, 2²⁸, 2³⁰ limbs | residue mod 2⁶¹−1 and length | exact |
| mdev_pair at 2¹², 2²⁰, 2²⁷+5 limbs | vs `mpz_mul` / residue | exact; B transformed once |
| batch: L = 2¹⁰ … 2¹⁸, N = 5 (CPU CRT) … 40 000 (several 15 GB tiles), all generators, registered and unregistered (staged) operands | vs `mpz_mul` per product | exact |
| grpB: 200 / 3 000 A against one B, 2¹⁴ / 2¹⁷, registered and staged, CPU-CRT size | vs `mpz_mul` | exact |

**One mdev multiply at 2³¹ points (2 × 2³⁰ limbs), steady state, seconds:**

| repack | H2D + canon | fwd × 2 | inv (+pw) | store | CRT | total |
|---:|---:|---:|---:|---:|---:|---:|
| 0.75–0.80 | 0.025 | 0.265 | 0.134 | 0.012 | **0.74–1.6** | **1.9–2.8** |

* The NTT part is 0.40 s = 19 % (as §28 predicted); the CRT is 40–55 % and
  noisy; the repack (two 8 GiB copies into each of four nodes) is 0.8 s.
* First use of a result buffer adds ~1–2 s of page faults (16 GiB) — the
  driver must keep result buffers grow-only, as the paper says.
* **Q1 (i) measured:** 2.08 × 10⁹ × 2.08 × 10⁹ limbs (the 10dP product,
  4.16 × 10⁹ > 2³¹ points) through the Karatsuba split: **9.1 s** (3 mdevs =
  6.3 s; host adds, subtracts and copies 2.8 s), peak RSS 204 GB. Against the
  paper's whole 10dP phase of 12.6 s (which also forms 10^d), consistent with
  (i). Exactly-2³¹-limb operands are pathological for the split (the middle
  term is two limbs too long and splits again: 5 mdevs, 54 s in a first
  version with fresh temporaries) — irrelevant to the paper's sizes, but the
  scratch is now grow-only per recursion depth and the limb add/sub are
  parallel.
* Batch throughput ≈ 1.0–1.3 Gpoint/s of product (2¹⁴ × 40 000: 0.57 s for
  6.6 × 10⁸ points; grpB 3 000 × 2¹⁷: 1.29). Not yet profiled; the paper's
  batched-NTT figures are per transform, not per product.

### Step 4 — `crt.h/.c`, `t_crt` (24 checks) and the GPU CRT
* CPU `crt_carry_par4`: Garner (`__int128 %`, rule 7) with the 3-limb carry
  window; `crt_carry_par4_q`: the same over a quartered, node-local layout.
  Exact vs GMP for random residues at T = 1, 7, 96, 192; all coefficients
  = M − 1 (every limb carries, every stripe spills); a single M − 1 straddling
  a stripe boundary; fewer coefficients than stripes; both layouts.
* GPU `k_crt_batch` (batch tier): one 256-thread block per product, each
  thread a coefficient range with the 3-limb window, LDS spill, sequential
  ripple merge by thread 0; Garner's modular steps by the FP64-Barrett modmul
  (a 128-/192-bit value mod p as two/three Barrett steps with 2⁶⁴ mod p
  precomputed); planes peer-read from all four devices; results stored
  straight into registered host memory. Exact on every batch case above.
* **Throughput at 2³⁰ coefficients, 192 threads, best of 3:**

| plane placement | 48 thr | 96 | 192 |
|---|---:|---:|---:|
| each plane first-touched by the stripe that reads it (bench/20's layout) | 1.13 s | 0.57 | 0.49 |
| one plane per NUMA node (mdev's staging) | 0.85 | 0.43 | **0.40** |
| quartered, node-local | 0.78 | 0.46 | 0.38 |
| registered hugepage planes, one per node, 5 runs | — | — | 0.42–0.66 |
| the same right after a nested 4 × 48 region | — | — | 0.40–0.50 |

The CRT is **compute-bound at 0.4–0.65 s per 2³⁰** whatever the placement
(the quartered layout, `RNS_CRT_LAYOUT=1`, buys nothing and costs 0.15 s of
remote stores; the paper's plane-per-node layout is the default). §34's
0.29 s was a best case; the in-situ figure is 0.75–1.5 s per 2³¹ with a
2× run-to-run spread that survives placement changes, thread counts and
nested-region history — CPU frequency under 192-thread load is the remaining
suspect. **The CPU CRT is 40–55 % of an mdev multiply; the GPU peer-gather
CRT (§28, Phase 5) is the lever.**

### Phase 3 model after steps 0–4
One 2³¹-point multiply-equivalent ≈ 2.0–2.5 s (paper's implied 2–3 s). The
10dP product ≈ 9 s plus ~2 equivalents for 10^d ≈ 14 s vs 12.6 s; dm at
≈ 20 equivalents ≈ 45 s vs 46.8 s. On track for the paper's 285.7 s
provided bs and dc (steps 6–7) land near their Table I times.

## 39. Phase 3 — steps 5–8: Newton division, binary splitting, radix conversion, verification, driver (2026-09-13)

### Step 5 — `newton.h/.c`, `t_newton` (696 checks)
Limb-level Newton doubling from a 2-limb schoolbook seed (Knuth D on the top
four limbs of Q, divisor rounded up): r' = (r << (64j + 1)) − (Q_t r² >> 64·take),
take = min(2j + 2, nq). Two guards: the paper's overshoot (T₂ < T₁: r −= r/16,
retry) and a new one — a step whose correction is not below 2^(64 j) repeats
at the same precision instead of doubling, which makes the iteration converge
from any seed (a 25 % perturbation otherwise ends far off and the division's
correction loop runs away; that loop is now bounded at 64 and aborts).

| check | result |
|---|---|
| `bi_divmod_school` vs `mpz_tdiv_qr`, 400 random sizes ≤ 64 limbs, all generators | exact |
| reciprocal error vs ⌊2^(64(nq+k))/Q⌋, nq = 5 … 2²⁰+1, k = 1, nq/2, nq, 2nq, four generators | **0 units in every case** |
| `newton_divmod` vs `mpz_tdiv_qr`, nq = 2¹⁰ … 2²⁶: random, all-ones, na = nq+1, A < Q, Q = 2ᵏ, 2ᵏ−1, A = Q², Q² ± 1, overshoot seed (2.5×), low seed (0.75×), supplied longer μ | exact; 0 down- and ≤ 2 up-corrections per division |
| 2²⁷ / 2²⁶ limbs (residue check) | 7.2 s (reciprocal 6.2 s, 26 iterations, 39 mdevs), 0 corrections |

Not implemented (Phase 5 knobs): the paper's `NEWTON_R2TRUNC` (truncated r²
on the last iteration) and the high-product-only A·μ; full products are used.

### Step 6 — `binsplit.h/.c`, `t_bs` (10 checks)
Seed spans of 512 terms (OpenMP), then levels with the tier chosen by the
largest node: schoolbook (≤ 160 limbs), one `rns_mul_batch` of 2·npairs
products (≤ 2^22 points), else `rns_mul_pair` per pair (Q₂ transformed once).
Nodes live in two alternating registered pools (grow-only) so the batch tier
reads them in place.

| check | result |
|---|---|
| P, Q vs the CPU recursion, N = 10³, 10⁵, 10⁶, with thresholds lowered (seed 8, school 4) so every tier runs, and at the defaults | exact |
| e to 10⁶ and 10⁷ digits through GMP division + `mpz_get_str` | SHA-256 == `ref/` |
| N = 1.3 × 10⁸ (10⁹ digits): 254 303 spans, 18 levels (14 batch, 4 mdev) | **4.5 s** (seeds 0.24, batch 1.7, mdev 2.3); pools 2 × 1 GB |

### Step 7 — `todec.h/.c`, `t_dec` (28 checks)
Digit count padded to Lg·2^m (Lg = 18u, 128 ≤ u < 256) so each level has one
divisor T = 10^h and one cached Newton reciprocal; divisor cache prewarmed
bottom-up by squaring (unseeded reciprocals — the paper's μ-square seed is a
Phase 5 item). TOP (≤ 2 pieces) and MID (pieces above the batch limit):
`newton_divmod` per piece with the cached μ; DEEP: two grpB batches per level
(A·μ against one μ, X·T against one T) and ±T corrections on the CPU; LEAF: a
GPU kernel per piece (< 256 limbs) dividing by 10¹⁸ with the paper's
μ = ⌊2¹²³/10¹⁸⌋ Barrett and writing 18-digit blocks straight into the
registered output string.

| check | result |
|---|---|
| random X < 10^n, n = 10³ … 10⁶, five generators, leaf size varied so TOP/MID/DEEP/LEAF all run, leading zeros | == `mpz_get_str` |
| e to 10⁶, 10⁷, 10⁸, 10⁹ from `ref/` (X by `mpz_set_str`) | **byte-identical** |
| 10⁸ digits (15 levels, 32 768 leaves) | 5.7 s (GMP `mpz_get_str` 18.8 s) |
| **10⁹ digits** (18 levels, 262 144 leaves of 3 816 digits) | **18.4–34.7 s**: prewarm 7.4, TOP 1.4, MID 4.7, DEEP 4.5, LEAF 0.4 (GMP 277 s) |

MID was 21.6 s in the first run: per-piece mdevs of 2¹⁸–2²⁴ points each cost
~20 ms of fixed overhead (nested 4 × 48-thread repack teams). Fixed by a
memcpy fast path below 2²⁰ limbs and by raising the batch limit to 2²² points.

### Step 8 — `verify.h/.c`, `t_verify` (334 checks), `ecalc`
T1: eight primes just above 2⁶² (results/0_params.txt); P, Q mod q by the
recursion in ℤ/q (768 chunks, serial combine), X, R, T mod q by Horner over
limbs, and — beyond the paper — the digit string mod q by Horner over digits,
so dc is residue-checked too, not only windowed. Checks: T(P+Q) ≡ XQ + R and
digits(X) ≡ X. T2: 50-digit windows at 50, 10⁶, 10⁸, 10⁹−49 (from `ref/`).
A flipped bit in X (low, middle or top limb), in R, or one changed digit is
caught by all eight primes; genuine GMP values pass.

`ecalc <digits> [outfile]`: bs → 10dP → dm → T1 → dc → T2 with per-phase
timers, `META`/`RESULT` lines and VmHWM.

| digits | bs | 10dP | dm | T1 | dc | T2 | compute | total (incl. 12 s init) | result |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10⁶ | 0.14 | 0.02 | 0.08 | 0.02 | 0.57 | 0.01 | 0.8 s | 13.5 s | identical to `ref/` |
| 10⁷ | 0.34 | 0.05 | 0.17 | 0.05 | 0.75 | 0.03 | 1.4 s | 13.1 s | identical |
| 10⁸ | 0.74 | 0.21 | 1.05 | 0.05 | 3.96 | 0.05 | 6.1 s | 18.8 s | identical |
| **10⁹** | 4.5 | 0.8 | 5.2 | 0.5 | 18.5 | 0.5 | **31 s** | 43–46 s | **identical to `ref/e_1000000000.txt`; T1, T2 pass** |

Peak RSS 80 GB at 10⁹ (64 GB of it the pregrown staging). Init is 4 × 1.2 s
of `hipHostRegister` plus pool growth — the paper's numbers exclude it too.

### Where this leaves the 4 × 10¹⁰ target
Scaling the 10⁹ phases by the paper's ratios does not work directly (the
top levels change tier), but a component estimate from §38 and the level
logs gives: bs ≈ 160 s (14 batch levels at ~1.1 Gpoint/s ≈ 100 s + 9 mdev
levels ≈ 60 s), 10dP ≈ 14 s, dm ≈ 60 s (27 mdev-equivalents), dc ≈ 250 s
(unseeded prewarm ≈ 40 s, five mdev-size top levels ≈ 50 s, 18 batch levels
≈ 150 s) — **≈ 490 s against the paper's 285.7 s**, with the gap in the
batch tier's product throughput and the unseeded divisor cache. Both are
known, bounded items (step 9 / Phase 5), not correctness risks: every
component is exact and verified, and the pipeline is faithful to the paper's
structure with three documented substitutions (kernel store instead of D2H
memcpy, GPU Garner via FP64 Barrett, digits residue-checked in T1).

### Step 9 — first 4 × 10¹⁰ run (2026-09-13, `results/ecalc_4e10.txt`, `results/e_4e10.out` 40 GB)
Two attempts were OOM-killed in dm (host RSS 377 GB + 128 GB device pools);
fixed by forming the Karatsuba partial products in place, releasing the
split scratch and Newton temporaries between phases, and computing μ before
A so P, T and S are gone at the division's peak. Third attempt:

| phase | ours | paper A22 | notes |
|---|---:|---:|---|
| bs | 241.7 s | 112.2 | seeds 7.0, batch 49.3, **mdev-pair levels 176.5** (10 levels, ~17 s each vs ~7 ideal) |
| 10dP | 15.6 | 12.6 | A = T(P+Q): 3 mdev, 1 split |
| dm | 129.6 | 46.8 | reciprocal 86.0 s (30 iterations, 55 mdev, full products); division 10 mdev; 0 corrections |
| T1 | 5.4 | ~3 | pass |
| dc | 550.6 | 110.3 | prewarm 67.7, TOP 39.3, **MID 300.9**, DEEP 123.4, LEAF 12.8; 24 levels, 16.8 M leaves |
| T2 | 11.6 | — | windows at 50, 10⁶, 10⁸, 10⁹ pass; digits ≡ X mod all eight primes |
| **total** | **1 020.9 s** | **285.7** | **verified; 3.6× the paper**; peak RSS 361.8 GB (paper 256) |

Every component is exact and the result is verified; the acceptance
criterion "each phase within ~15 %" is not met. The gap is concentrated:
dc's MID tier (per-piece Newton divisions above the 2²² batch limit, each
paying the mid-size mdev fixed cost), bs's mdev-pair levels, the unseeded
divisor prewarm, and full-product Newton. Next: batched division for the dc
MID levels, μ-square seeding of the prewarm, then the per-mdev overhead and
the GPU CRT (Phase 5).

### Step 9b — tuning pass 1 (2026-09-14, `results/ecalc_4e10_v2.txt`)
1. **Striped GPU CRT** for the batch tier (S stripes per product spread over
   all four devices, spills merged on the CPU) so batches take products up to
   2³⁰ points; T and μ of the divisor cache in registered memory; dc's MID
   levels now run through the batched DEEP path.
2. **Seeded prewarm**: μ(2h) from μ(h)² (at μ(h)'s precision) plus one
   doubling — the paper's scheme.
3. **Coalesced CRT kernel** (chunks of 256 consecutive coefficients through
   LDS): per-batch CRT 0.12 → 0.01 s, batch throughput 1.2 → 4.25 Gpoint/s;
   the same kernel now does mdev's CRT (planes peer-read, result into device
   0's staging, one parallel copy): the CPU Garner at 2²⁴–2²⁷ points ran at
   only 0.67 Gcoef/s. dc's remainder step over an nl_next + 2-limb window.
   A full-width repack copy was tried and rejected (1.20 vs 0.78 s at 2³¹).

| | 10⁹ | 10¹⁰ | **4 × 10¹⁰** | paper |
|---|---:|---:|---:|---:|
| bs | 4.5 → 2.0 | 48 → 15 | 242 → **74** | 112 |
| 10dP | 0.8 → 0.5 | 3.1 → 2.8 | 16 → 11 | 12.6 |
| dm | 5.2 → 4.2 | 23 → 20 | 130 → **119** (reciprocal 77) | 46.8 |
| T1 | 0.3 | 1.3 | 5 | ~3 |
| dc | 18.5 → 5.7 | 131 → 40 | 551 → **155** (prewarm 33, TOP 27, DEEP 77, LEAF 10) | 110 |
| T2 | 0.3 | 2.8 | 27 | — |
| **total** | 31 → 13 s | 211 → 95 | **1 021 → 456 s** (429 without T2) | 285.7 |
| peak RSS | 80 | 142 | 355 GB | 256 |

Verified at every size (10⁹ byte-identical to `ref/`; T1, T2 and digit
residues at 10¹⁰ and 4 × 10¹⁰). **bs and 10dP are now faster than the
paper**; dc is 1.4× and dm 2.5× slower. What is left: the reciprocal (77 s:
55 mdevs, the top two iterations through 2³²-point split products with full
r² and Q_t·r²), dc's TOP levels (27 s, two mdev-based divisions per level)
and prewarm (33 s), and the mdev repack (0.78 s of a 1.6 s 2³¹ multiply).

Not done from the plan: **step 4** (truncated r² / high-product step). As
specified in the paper it is not sound at limb level — dropping L low limbs of
r² perturbs T₁ by up to 2^(64L) units, which is exactly the new precision the
step is computing; the paper's `skip`/`take` bookkeeping must carry an error
analysis it does not print. A middle-product formulation would be the correct
route (Phase 5). **Step 5** (memory): 355 GB vs 256; the remaining excess is
the in-place split's z1 and the Newton temporaries at 2³² points (~60 GB) and
dc's registered scratch (~25 GB).

### Step 9b — tuning pass 2 (2026-09-14, `results/ecalc_4e10_v4.txt`)
Parallel limb shifts/copies (Newton's in-place shifts made out-of-place),
parallel node normalisation in bs, dc's remainder via the parallel limb
primitives, the DEEP scratch sized once for the whole conversion (it was
re-registered at nearly every level: 0.6 s each), parallel copies in dc's
TOP levels and the divisor cache. Tried and rejected: a full-width repack
copy (1.20 vs 0.78 s at 2³¹). Not done: the middle-product Newton step
(estimated ≈ 5 s of dm; the paper's remaining dm advantage is larger than
that and must come from its truncated iterations).

| phase | pass 1 | **pass 2** | paper A22 | ratio |
|---|---:|---:|---:|---:|
| bs | 74 | **72.0** | 112.2 | 0.64 |
| 10dP | 11 | **9.1** | 12.6 | 0.72 |
| dm | 119 | **74.0** (reciprocal 44.0) | 46.8 | 1.58 |
| T1 | 5 | 4.3 | ~3 | — |
| dc | 155 | **111.9** (prewarm 23.7, TOP 24.5, DEEP 43.2, LEAF 11.3) | 110.3 | 1.01 |
| T2 (not in the paper's total) | 27 | 25.6 | — | — |
| **total without T2** | 429 | **337 s** | **285.7** | **1.18** |
| peak RSS | 355 | 355 GB | 256 | 1.39 |

Verified (T1, T2 windows, digit residues); 10⁹ still byte-identical to
`ref/` (compute 11 s, from 31). The sequence over the day: 1 021 → 456 →
358 → 337 s. Against PLAN §9's acceptance (each phase within ~15 %): bs,
10dP and dc pass; dm does not (1.58×); memory does not (355 vs 256 GB).

### Step 9b — tuning pass 3 (2026-09-14, `results/ecalc_4e10_v5.txt`)
T2's digit residue in 18-digit steps (26 → 4.6 s). Newton in the correction
form r' = (r ≪ 64j) + ((r·d) ≫ 64j), d = 2^(128j) − (Q_t·r ≫ 64(take − j)):
fewer and smaller products (34 vs 39 mdevs at 2²⁶, 1.48 vs 1.96 s; still 0
units of error), but at 4 × 10¹⁰ the reciprocal measured 47.6 s vs 44.0 —
within the run-to-run spread (bs 72–76, dc 109–120 s between identical
runs), so no gain is claimed there.

| phase | pass 2 | pass 3 | paper |
|---|---:|---:|---:|
| bs | 72.0 | 75.7 | 112.2 |
| 10dP | 9.1 | 10.2 | 12.6 |
| dm | 74.0 | 86.2 (reciprocal 47.6, division 38.6) | 46.8 |
| dc | 111.9 | 109.2 | 110.3 |
| T2 | 25.6 | **4.6** | — |
| total without T2 | 337 | 351 | 285.7 |
| peak RSS | 355 | 350 GB | 256 |

Verified. The pass-2/pass-3 difference is noise except T2. Items with a
clear return are exhausted; dm is the phase outside the paper's envelope and
closing it needs the paper's unstated Newton details (or a middle-product
formulation) rather than tuning.

## 40. Phase 4 — verification and acceptance (2026-09-14, `ecalc/accept.sh` → `ecalc/results/phase4/`)

One recorded sweep on `ppac-pl1-s24-16` (job 20500, 44 min): every unit and
integration test, then the end-to-end ladder with every check.

| level | test | result |
|---|---|---|
| unit | `t_params` (constants), `t_modarith` (125), `t_ntt` (452), `t_mul` (109 + 16), `t_crt` (24), `t_newton` (696), `t_bs` (10), `t_dec long` (28), `t_verify` (334) | **all VERIFY OK** |
| end-to-end | e to 10⁶, 10⁷, 10⁸, 10⁹ digits | **byte-identical to `ref/e_<d>.txt`**; T1, T2 pass |
| end-to-end | e to 10¹⁰ | T1, T2 windows, digits ≡ X mod 8 primes: pass; 87 s; 141 GB |
| end-to-end | **e to 4 × 10¹⁰** | **T1, T2 windows, digits ≡ X mod 8 primes: pass**; 323 s; 350 GB |

### Performance against the paper (Table I, A22)
| phase | ours (s) | paper (s) | ratio | within 15 %? |
|---|---:|---:|---:|---|
| bs | 72.3 | 112.2 | 0.64 | yes (faster) |
| 10dP | 10.0 | 12.6 | 0.79 | yes (faster) |
| dm | 69.1 | 46.8 | 1.48 | **no** |
| T1 | 4.8 | ~3 | — | — |
| dc | 96.9 | 110.3 | 0.88 | yes (faster) |
| **total without T2** | **316.8** | **285.7** | **1.11** | no (by 11 %) |
| T2 (not in the paper's total) | 6.2 | — | — | — |
| peak RSS | 350 GB | 256 GB | 1.37 | **no** |

> **Correction (2026-09-18, found in WP3, §56):** the "total" rows in
> §40–§42 are wall time from program start. They include init (11–15 s:
> staging touch/register, device pools) and a linear `lgamma` scan in
> `e_terms` (≈ 45 s at 4 × 10¹⁰) that no phase timer covered. The sum of
> the phases in the accepted runs was **≈ 229 s (0.80 × the paper's
> 285.7 s)**; the 281–290 s wall figure is what was compared. Both are
> stated here; from §56 on the run prints phases, init and other
> separately, and the scan is a bisection.

### Verdict
The paper's **results are reproduced**: the algorithm as described, on the
stated hardware class, produces the correct 40 billion digits (verified by
the paper's own tier-1/tier-2 scheme plus a digit-string residue check) with
per-phase times within 15 % of — or under — Table I for bs, 10dP and dc, and
a total within 11 %. Two acceptance criteria are **not met**: dm at 1.48×
(the reciprocal: the paper's truncated Newton step is not sound as printed,
§39; its 46.8 s needs a formulation the paper does not give) and peak RSS at
350 vs 256 GB (the split's temporaries at 2³² points and dc's scratch).
Neither affects correctness. Run-to-run spread on identical runs is ≈ ±5 %
(dc 97–120 s across four runs), which the comparison above should be read
against.

Substitutions relative to the paper, all documented: kernel store instead of
D2H memcpy; striped, coalesced GPU CRT for both batch and mdev (the paper's
CPU Garner for mdev); Karatsuba over halves for products above 2³¹ points
(Q1 (i)); Newton in correction form with a self-correcting doubling; digit
string residue-checked in T1. **Phase 4 closed; Phase 3 accepted with the
two exceptions above carried into Phase 5.**

### §40 addendum — the two exceptions closed (2026-09-14, `results/ecalc_4e10_v6..v8.txt`)
* **dm:** the division forms only the top half of A·μ (the low nq−1 limbs of
  A are under one unit of X; the ±1 is absorbed by the existing corrections)
  and X·Q as a low product mod 2^(64(nq+2)) (`rns_mul_low`, split so nothing
  above the window is formed; 24 new GMP checks across every split shape).
  Six mdevs instead of ten and no 50 GB product. dm **48.3–49.2 s vs 46.8**
  (1.04×). dc's TOP levels use the same division: dc 80–82 s.
* **memory:** the 40 GB digit string and dc's 25 GB DEEP scratch are
  allocated at first use; X is freed once dc has copied it (its residues are
  taken first); the Newton and split scratch grown by dc's prewarm is released
  before the levels. Peak RSS 338 → 293 → 269 → **248 GB** (now the
  reciprocal's transient in dm), under the paper's 256.

| phase | ours (3 runs) | paper | ratio |
|---|---:|---:|---:|
| bs | 70.9–77.4 | 112.2 | 0.63–0.69 |
| 10dP | 9.0–10.0 | 12.6 | 0.71–0.79 |
| dm | 48.3–49.2 | 46.8 | 1.03–1.05 |
| T1 | 4.6–7.6 | ~3 | — |
| dc | 79.8–81.5 | 110.3 | 0.72–0.74 |
| **total without T2** | **281.4–289.9** | **285.7** | **0.98–1.01** |
| peak RSS | 248 GB | 256 | 0.97 |

Verified on every run (T1, T2 windows, digit residues; 10⁹ byte-identical).
**All Phase 4 acceptance criteria are met**: digits agree at 10⁹; T1 and T2
at 4 × 10¹⁰; every phase within 15 % of Table I (three of them faster); peak
RSS ≤ 256 GB. The reproduction runs at the paper's speed on the paper's
memory budget.

## 41. Phase 5 — experiments (2026-09-14, partial)

Phase 5's items are alternative engines behind the Phase 3 interfaces; two
are settled here by measurement or arithmetic, the rest need their own
builds (estimates below).

### Q1 (ii) — 2³² pools, no split (`t_mul 0 big32`, `POOL_LOG=32`)
The 10dP-size product 2.08 × 10⁹ × 2.08 × 10⁹ limbs:

| | pools | product | time (2nd run) | device memory |
|---|---|---|---:|---:|
| (i) default | 2³¹ | Karatsuba: 3 mdevs of 2³¹ + host adds | 6.88 s | 128 GiB |
| (ii) | 2³² | one mdev of 2³² (fwd 0.52, inv 0.27, repack 1.09, store 0.42, CRT 0.47) | **2.68 s** | **256 GiB** |

(ii) is 2.6× faster for that one product but doubles the device pools; with
the pipeline's 248 GB host peak it does not fit the node's 512 GB, and it
contradicts the paper's stated 128 GB of device pools. The pipeline has only
one 2³²-point product left (A = T·(P+Q); the division now uses half-size
products), so (ii) would save ≈ 4 s of 285. **Decision (i) stands; (ii)
stays as the `POOL_LOG=32` switch.**

### Q2 — b = 48 with 3 primes (arithmetic, no build)
With b = 48 a number needs 4/3 as many points; three planes of 4/3 n points
hold exactly the bytes of four planes of n points, and the transform work
(3 × 4/3 = 4 plane-transforms of equal size) is also identical. PLAN §10's
"25 % less plane memory" was wrong: b = 48/3 primes changes neither memory
nor compute; its only property is that three primes satisfy the convolution
bound (2·48 + 33 = 129 < 156) where three at b = 64 do not. **No experiment
needed; dropped.**

### Item 1 — Shoup u64 modmul in the paper's kernels (analysis)
The paper's b16 kernel forms each twiddle on the fly as tab[r] · T_H (one
modmul) and the b1 kernel from a 512-entry table. Shoup's modmul needs the
precomputed w' = ⌊w 2⁶⁴/p⌋ for every multiplier, so the on-the-fly twiddles
would have to become full per-pass tables (n/2 entries × 16 B read per pass,
+50 % memory traffic on a kernel that is LDS/modmul-bound at 1.2 TB/s, §25),
or the b1 pass alone could use Shoup (its tables are static). bench/01's
1.14× per butterfly against that extra traffic makes the whole-kernel result
uncertain and small either way. **Not built; the b1-only variant is the
cheap experiment if wanted (½ day).**

### Items 3, 4, 5, 6 — need builds
| item | what it takes | estimate |
|---|---|---|
| 3 register-blocked b16 body | port bench/06's 2048-point row kernel to FP64 Barrett arithmetic behind `ntt_fwd/inv`; `t_ntt` as the gate | 2 d |
| 4 one-prime-per-device vs four-step corner turn | a four-step `rns_mul_mdev` variant with the corner turn over xGMI (bench/03/11 kernels), CRT unchanged | 3 d |
| 5 2 × 62-bit primes, 45-bit repack (2.81 vs 2.0 bits/byte) — the path past 4 × 10¹⁰ on this node | general repack, 2-prime Garner, Shoup kernels from bench/06/07/12 behind the same interfaces; every Phase 3 test re-run | 1 wk |
| 6 radix-4 stages; negacyclic NTT for Karp–Markstein | kernel variants; the paper lists both as future work | 2 d |

Item 5 is the one that matters for the project's own goal (10¹¹ digits on
460 GiB) and should be done first when Phase 5 resumes.

## 42. Phase 5 task 1 — run-to-run variance at 4 × 10¹⁰ (2026-09-14, `ecalc/variance.sh`, `results/variance/`)
Five identical runs on `ppac-pl1-s24-16` (job 20541), `amd-smi` sampled every ~2.5 s; all five VERIFY OK, peak RSS 248.3 GB each.

| | bs | 10dP | dm | T1 | dc | T2 | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| mean (s) | 74.7 | 9.3 | 51.1 | 5.2 | 82.5 | 2.1 | **291.2** |
| sd (s) | 1.6 | 0.3 | 1.4 | 1.4 | 0.2 | 0.9 | 3.5 |
| sd (%) | 2.2 | 3.3 | 2.8 | 26 | 0.3 | 41 | **1.2** |
| paper | 112.2 | 12.6 | 46.8 | ~3 | 110.3 | — | 285.7 |

* The spread is 1–3 % on the real phases; only the two-to-seven-second
  verification phases vary by more (they are CPU-side and contend with the
  OpenMP pool's wind-down). Earlier "±5 %" readings were between *different*
  builds. Total without T2: **289.1 ± 3.5 s, i.e. 1.01× the paper**; dm is
  1.09× (its sd covers the difference), bs 0.67×, dc 0.75×.
* Per-APU mean shader clock over a run (busy and idle mixed) is 1 205–1 220
  MHz on all four APUs within ±1 %, mean socket power 232–250 W of the 550 W
  cap; run 5's APU0 reads 1 683 MHz from idle up-clocking during CPU phases.
  **No per-APU asymmetry**; the D13 "20 % per-APU spread" of Phase 1 is not
  present in the pipeline. The variance is not clock-driven — it sits in the
  host-side phases (bs, dm) and is small enough that a 2 % kernel change is
  measurable with two runs.

## 43. Phase 5 tasks 2–3 — register-blocked b16 body and radix-4 stages (2026-09-14)

`ntt.c` `k_b16r<INV, R4>`, selected by `NTT_B16_BODY` (0 tile kernel, the
paper's; 1 register-blocked; 2 register-blocked with radix-4 stages), for the
7-stage passes; shorter passes stay on the tile kernel.

**Body 1.** Each thread keeps 8 rows of its column in registers and runs the
seven stages as three groups (h = 64/32/16 on rows tt + 16i; h = 8/4/2 on
rows ((tt≫1)≪4)|(i≪1)|(tt&1); h = 1 on rows 8tt + i) with two LDS
exchanges instead of one round trip per stage. Same butterflies, folds and
twiddle products, so the output is **bit-identical** (`t_ntt` hash check).

**Body 2.** Within a register group, two stages as one radix-4 step: the
stage-H pair (b, d) uses w·ω₄ and both stage-H/2 pairs use w², so three
twiddle modmuls replace four; the butterfly sequence is unchanged, so still
bit-identical.

| 2³¹, per APU | body 0 (paper) | body 1 | body 2 |
|---|---:|---:|---:|
| forward | 114.7 ms, 1.20 TB/s | **95.1 ms, 1.44 TB/s** | 94.9 ms, 1.45 |
| inverse (+ n⁻¹) | 118.6 ms, 1.16 | **103.6 ms, 1.33** | 103.4 ms, 1.33 |
| batched log L = 14 (4-stage passes: tile kernel) | 1 579 GB/s | 1 586 | — |
| **4 × 10¹⁰ pipeline** | 291.2 ± 3.5 s (5 runs) | **285.5 s** (one run) | — |

* The register-blocked body is **+19 % forward, +15 % inverse** on the
  long transforms: the tile kernel's LDS round trip per stage was the
  limiter, as §25 suggested. In the pipeline the long passes are a small
  share (bs and dc are batched at log L ≤ 22, mdev's NTT is ~0.4 s of
  1.6 s), so the whole run gains ≈ 2 %, at the edge of the run-to-run spread.
* Radix-4 buys **nothing measurable** (+0.4 %): with the LDS traffic
  removed by body 1, the kernel is no longer twiddle-modmul-bound — the
  remaining time is the global loads/stores and the data modmuls.
* Both are bit-identical to the paper's kernel and pass all 458 `t_ntt`
  checks. `ecalc` keeps body 0 (the paper's) as its default so the accepted
  numbers stand; `NTT_B16_BODY=1` is the recommended switch and is what the
  Phase 5 engine comparisons will use. The paper's "radix-4 stages" future
  work item is answered: it is worth ≈ 12 % of twiddle modmuls on paper and
  nothing in practice once the pass body keeps its rows in registers.

## 44. Phase 5 item 5 — engine 2: two 62-bit primes, 45-bit points (2026-09-15)

Built behind the same interfaces (`RNS_ENGINE=2`): `modarith2.h` (P1, P2
from `~/ntt`, Montgomery arithmetic so twiddles are still formed on the fly;
values lazy in [0, 2p) with no folds needed), `ntt2.c` (the same tiled and
register-blocked kernels, a 45-bit repack in the load), `crt2.c` (Garner for
two primes with the 45-bit carry), and engine-2 mdev / pair / batch tiers in
`rns_mul.c` (device d: prime d & 1, product half d ≫ 1; CPU CRT only).
Convolution bound 2·45 + 33 = 123 < log₂ P1P2 = 123.6.

| test | result |
|---|---|
| `t_ntt2`: Montgomery product vs 128-bit (4 M pairs), DFT at 2¹⁰–2¹³, round trips 2¹⁰–2²⁰, 45-bit product vs `mpz_mul` at 2¹⁴–2²² points | 35 checks OK |
| `t_mul 20` with engine 2: every tier and split | 133 checks OK |
| `ecalc` 10⁸, 10⁹ | byte-identical to `ref/` |
| `ecalc` 10¹⁰ | verified; 172 s vs engine 1's 87; peak 120 vs 131 GB |
| **`ecalc` 4 × 10¹⁰** | **verified; 641 s vs 291; peak 282 vs 248 GB** (bs 222/75, dm 106/51, dc 225/82) |

Findings:
1. **The density argument does not apply to this pipeline.** In the paper's
   structure the numbers live on the host as 64-bit limbs (8 bits/byte) and
   the NTT planes are transient device buffers; 2.81 vs 2.0 bits per plane
   byte changes how many limbs one plane holds (1.5 × 10⁹ vs 2.1 × 10⁹ at
   2³¹ points), not the host peak. Engine 2's peak is *higher* because more
   products exceed a plane and go through the split's temporaries. The
   2.81 bits/byte figure only sets the ceiling in a memory-resident-plane
   design like `~/ntt`'s ALGORITHM.md — a different pipeline, not a switch.
2. As built engine 2 is 2.2× slower: one product occupies two devices (two
   primes), so mdev uses half the node, and its CRT is CPU-only. Both are
   fixable (products in pairs on device pairs; a 2-prime striped GPU CRT)
   but cannot beat engine 1's per-multiply time by more than the 0.71×
   plane-transform work, i.e. ≈ 10 % of a run.
3. **Decision: engine 1 (the paper's) stays the engine of this pipeline.**
   The digit ceiling on this node is set by host memory: 248 GB at 4 × 10¹⁰
   plus 128 GiB of device pools in the same 512 GB → ≈ 5 × 10¹⁰ digits with
   the current footprint (task 2 measures it).

**CORRECTION to ALGORITHM.md:** its "3.4 bytes per digit → 10¹¹ digits"
estimate assumes memory-resident planes and does not carry over to the
paper's host-resident pipeline; on that pipeline the ceiling is ≈ 6.2 bytes
per digit (248 GB / 4 × 10¹⁰) plus the fixed 192 GB of pools and staging.

## 45. Phase 5 task 2 — past the paper: e to 5 × 10¹⁰ digits (2026-09-15, `results/ecalc_5e10.txt`, `results/e_5e10.out`)
Engine 1, register-blocked body, T2 windows from the verified 4 × 10¹⁰ file
(`ref/windows_4e10.txt`: 10¹⁰, 2 × 10¹⁰, 3 × 10¹⁰, 4 × 10¹⁰ − 49).

| phase | s | notes |
|---|---:|---|
| bs | 99.2 | N = 5.38 × 10⁹ terms; P, Q 2.6 × 10⁹ limbs; level pools 45 GB |
| 10dP | 17.7 | A = 5.19 × 10⁹ limbs: 8 mdevs, 4 splits |
| dm | 140.0 | reciprocal 111 s (31 iterations, 67 mdevs — Q is over 2³¹ points now, so its products split) |
| T1 | 5.6 | pass |
| dc | 99.6 | 24 levels, 16.8 M leaves |
| T2 | 1.8 | **4 windows from the 4 × 10¹⁰ digits ok; digits ≡ X mod 8 primes** |
| **total** | **441.7** | **VERIFY OK; peak RSS 375.4 GB** |

375 GB host + 128 GiB device pools = 503 GB of the node's 512 GB: **5 × 10¹⁰
is the ceiling of this pipeline on one MI300A node** without shrinking the
pools (POOL_LOG = 30 would trade ≈ 20 % time for ≈ 64 GB). Scaling from
4 × 10¹⁰: time × 1.52 (dm grows fastest, 2.7×, because Q crosses the plane
size), memory × 1.51.

## 46. Phase 5 item 4 — one prime per device vs four-step corner turn (analysis, 2026-09-15)
For a product of n points and four primes the device memory is 2 arrays ×
4 planes × n × 8 B whatever the distribution: one prime per device (the
paper) puts one full plane on each device; a four-step split puts a quarter
of every plane on each device — the same 8n bytes per device. The premise
in PLAN §10 ("halves per-device plane footprint") was wrong. A 2³²-point
four-step multiply therefore needs exactly Q1 (ii)'s 256 GiB of pools, which
§41 showed does not fit beside the host footprint; and at 2³¹ points the
corner turns (three all-to-alls per multiply at 909 GB/s, RESULTS §21) can
only add time to a multiply that is already 25 % NTT. **Dropped; the
paper's structure is the right one for this node.** The four-step remains
the multi-node path (the paper's own future-work item).

## 47. Phase 5 item 1 — Shoup integer modmul in the b1 pass (2026-09-15)
`NTT_B1_SHOUP=1`: the b1 pass (stages 9..0, static 512-entry table) with
Shoup's modmul (w' = ⌊w 2⁶⁴/p⌋ precomputed; d·w − ⌊w'd/2⁶⁴⌋p ∈ [0,2p) for any
64-bit d, so the lazy representation needs no folds). Output canonical and
bit-identical (`t_ntt`, 461 checks).

| 2³¹, APU0 | forward | inverse |
|---|---:|---:|
| register-blocked body, FP64 b1 | 96.0 ms | 103.0 |
| register-blocked body, Shoup b1 | 98.4 ms | 106.0 |

**Slightly slower (2–3 %).** bench/01's 1.14× per butterfly for Shoup does
not survive inside a real pass: the FP64 modmul's VALU work co-issues with
the integer adds and LDS traffic (§31), while Shoup's two `mul_hi` land on
the same integer pipe as the butterfly. Item 1 is closed: the paper's FP64
choice is the right one on this architecture.

## 48. Phase 6 — MI300A capability benchmarks (2026-09-15, `bench/{system,mem,fabric,arith,lds}/`, `results/phase6/`)
One program per group, run on `ppac-pl1-s24-16` (job 20546).

**system/launch** — launch 3.1–3.6 µs (throughput), launch + synchronize
9.5–11.5 µs, event record + sync 11–12 µs, two 100 ms kernels on two
streams overlap fully (2.00×), `hipMemcpy` D2H 1 KiB 11–13 µs, 1 MiB
90–127 µs. Per-call costs are what mdev's ~20 ms fixed overhead was *not*:
they are 100× smaller; the overhead was the nested OpenMP teams (§39).

**mem/sweep** — streaming read, one kernel, APU0:

| working set | GB/s | level |
|---|---:|---|
| 1–16 MB | 9 300–12 500 | L2 (4 MB) and near |
| 32 MB–256 MB | 3 850–4 800 | Infinity Cache (256 MB) |
| 512 MB–4 GB | 3 400–3 700 | HBM (single read stream; §2's 14 TB/s was 4 APUs × read+write) |

Registered 64 GiB host arena, one 8-byte touch per 4 KiB page: 791 M
touches/s (3.2 TB/s of page-touch traffic), per 2 MiB 694 M/s, per 64 B
1 560 M/s — **no TLB cliff at 64 GiB**; the pinned-arena design of the
pipeline is safe at the node's full memory.

**fabric/p2p** — per pair, one copy kernel, 4 GiB:

| pair | pull | push | bidirectional |
|---|---:|---:|---:|
| 0–1, 0–3, 1–2, 2–3 | 90–92 GB/s | 91–94 | 177–181 |
| 0–2, 1–3 | 90–92 | 91–93 | **107** |

Dependent remote store+load round trip 1 197 ns (local 539 ns). A single
copy kernel gets ~91 GB/s per direction; bench/03's 909 GB/s corner turn is
the aggregate of all links with wide stores in flight. The 0–2 / 1–3 pairs
share a link in the bidirectional case — the ring topology shows.

**arith/occupancy** — FP64 Barrett modmul chains: 1 240–1 370 Gmodmul/s
per APU for every threads/block (64–512) and chains/thread (1–8): the
modmul is issue-bound and insensitive to occupancy (D1's finding, §31,
generalised). Integer Montgomery (62-bit): 1 010–1 040 Gmodmul/s — 25 %
below FP64 Barrett, the same ranking as §47. FP32 FMA 85.9 TFLOP/s (the
122.6 datasheet figure scaled to the measured 1.5 GHz is 87.6), packed FP16
102.6 TFLOP/s.

**lds/occupancy** — LDS read+write traffic per APU: 35.8 TB/s at 3
blocks/CU (the NTT kernels' 17 KB tiles), 38.4 at 4 blocks/CU (16 KB, no
pad), **19.0–19.3 at 1 block/CU** (34–64 KB tiles): occupancy is worth 2×,
which is why TILE = 128 / pad 17 beat 256 (§33). 16-byte accesses:
45.3 TB/s (+18 %). This is the ceiling the register-blocked body (§43)
relieved: the tile kernel's 1.2 TB/s of HBM traffic carried 7 LDS round
trips per element.

Phase 6 items not covered by these five programs (allocator curves, HBM
stride/width sweep, `ds_swizzle`/DPP, the row-transform N sweep, 7-smooth L)
remain listed in PLAN §11; each is an hour's program in this harness.

## 49. Phase 6 — the remaining items (2026-09-15, `results/phase6/`)

**system/alloc** (APU0, seconds): `hipMalloc` 0.04 / 0.23 / 0.64 for 1 / 4 /
16 GiB, `hipFree` 0.001 / 0.012 / 0.047, `hipHostMalloc` 0.13 / 0.51 / 2.0,
single-thread first touch 0.08 / 0.31 / 1.27, `hipHostRegister` 0.07 / 0.30 /
1.17, unregister ≈ 0. All linear in size (≈ 40 ms/GiB for device
allocation, 73 ms/GiB for registration). The grow-only pool policy is
justified: a 16 GiB device buffer costs 0.7 s to re-create, a registered
host buffer 2.4 s including its touch.

**mem/stride** (4 GiB, APU0, useful GB/s read):

| width | contiguous | 64 B lane stride | 256 B | 4 KiB |
|---|---:|---:|---:|---:|
| 4 B | 3 019 | 228 | 110 | 103 |
| 8 B | 3 739 | 456 | 219 | 207 |
| 16 B | 3 829 | 907 | 439 | 414 |

Coalescing is everything: one useful word per 64-byte line costs 8× at 8 B
and the request rate saturates near 3.4 G lines/s (207 GB/s ÷ 64 B) for
scattered access whatever the width. 16-byte lanes gain 2–3 % contiguous and
2× when scattered. This is the number behind the batch CRT's 12× gain from
coalescing (§39).

**lds/xchg** (dependent chain per lane, APU0): `ds_bpermute` (`__shfl_xor`)
160 ns, DPP row shift 74 ns, `ds_swizzle` 77 ns, LDS store + sync + load
122 ns; throughput 2.9 / 6.3 / 6.0 / 3.8 T lane-exchanges/s. DPP and
`ds_swizzle` are 2× `bpermute` and 1.6× an LDS round trip for the fixed
patterns they support (row shifts, xor within 32 lanes) — the way to take
the last exchange out of the register-blocked b16 body (§43) if it is ever
worth 5 %.

**kernel/rowN** (batched forward transforms over 2³¹ points, APU0, GB/s
effective): log L = 10: 1 385, 11: 1 781, 12: 1 744, 13: 1 652, 14: 1 585;
the register-blocked body is identical here (short passes use the tile
kernel). R11's "N = 512 rows" hypothesis is answered by the shape of the
curve: throughput is flat from 2¹¹ to 2¹⁴ and lowest at 2¹⁰ (one pass, all
in the b1 kernel), so the four-step row length is free to be chosen for the
corner turn, not the kernel. The paper's 103 GB/s at log L = 11 is 17× below
this engine.

**7-smooth transform lengths** (analysis from the 4 × 10¹⁰ level logs):
power-of-two padding wastes **37 % of the points in dc's DEEP levels** (each
A·μ product is 1.5 × a piece, which sits just above a power of two at every
level) and 3.6 % in bs. Lengths 3·2ᵏ would cut dc's waste to 3.0 %
(5·2ᵏ adds nothing more) — worth ≈ 25 % of dc's 44 s DEEP time, ≈ 11 s of
the 285 s run. That needs a radix-3 pass in the engine (the 52-bit primes
have 2³³ | p − 1 only; a 3 | p − 1 prime set or the 62-bit primes' 2⁴⁰·3·5·7
would be required), so it is recorded as the one structural gain left on the
table rather than built. **Phase 6 complete.**

## 50. Phase 7 WP1 — decimal limbs as a run-time switch (2026-09-16, branch `wp1-decimal-base`)

`LIMB_BASE=10` selects base-10¹⁸ limbs (60 bits each) everywhere; the
default remains the paper's 2⁶⁴. What changed (git log on the branch):
every `bigint` primitive carries at B (adds, subtracts, `mul_1`, schoolbook,
`divmod_u64`, the parallel ripples), a base-10¹⁸ Knuth D for the Newton
seed, all bit shifts replaced by limb shifts or divisions (audit in the
commit), the CPU and striped GPU CRTs split each ≤ 206-bit Garner result
into four base-B digits with the LEAF kernel's Barrett step and carry in
base B, the harness's GMP bridges and generators are base-aware, and in
decimal mode the driver forms 10^d·(P+Q) as a limb shift plus one
`mul_pow10` and prints the digits straight from X's limbs — i.e. **10dP and
dc are bypassed** (WP2 falls out of the switch; the binary code is kept).
Engine 2 stays binary-only. Test oracles that assumed 2⁶⁴ (low product,
reciprocal, batch import, T1 Horner) were fixed on the way — each first
appeared as a "failure" of correct code.

**Correctness (both bases):** binary — `t_mul` 133, `t_newton` 658, `t_bs`,
`t_crt`, 10⁸ and 10⁹ identical to `ref/` (the paper's path is unchanged).
Decimal — `t_newton` 658 (reciprocal error 0 units at every size), `t_crt`
12, `t_bs` 10, `t_verify` 334, and **e to 10⁸ and 10⁹ byte-identical to
`ref/`** with T1, T2 and digit residues passing.

| 10⁹, one node | binary (Phase 4) | decimal |
|---|---:|---:|
| bs | 1.9 | 2.2 |
| 10dP | 0.5 | 1.1 (single-thread `mul_pow10`; parallelised since) |
| dm | 4.0 | 5.1 |
| dc | 4.9 | **0.2** (formatting) |
| compute total | ≈ 11 s | ≈ 9 s |

**Gate (2026-09-17, job 20614, s24-26):** decimal `t_newton 26` 696,
`t_bs` 10, `t_verify` 334; e to 10⁶, 10⁷, 10⁸, 10⁹ byte-identical to `ref/`
(T1, T2 OK); **e to 10¹⁰ verified in 84.5 s** (bs 23.3, 10dP 0.8, dm 30.2,
T1 1.4, dc 1.2, T2 0.5; VmHWM 150.5 GB). **4 × 10¹⁰: all six decimal runs
were killed (host OOM) in dm**, after bs 101.7–106.6 s (binary ≈ 96) and
10dP 2.6–3.0 s (binary ≈ 11); the last VmHWM printed before the kill was
271 GB (binary: 248 GB peak for the whole run). Decimal has 7 % more limbs
(59.8 bits per limb), which alone does not explain it; the run logs showed
why: `ecalc.c` sized the reciprocal prewarm with the binary formula
d·log₂10/64, so in decimal μ was 2.076 × 10⁹ limbs where 2.222 × 10⁹ were
needed, `newton_divmod` discarded it and recomputed the reciprocal with A
alive — the peak the prewarm exists to avoid. Fixed (`ff35352`, base-aware
limb count). (The first rerun on s24-16 executed a stale copy of the binary
from the shared filesystem and that node then failed; s24-16/30/35 are down.)

**4 × 10¹⁰ decimal, five runs on s24-26 (job 20630), all VERIFY OK:**

| phase | binary mean ± sd (Phase 4, §42) | decimal mean ± sd | ratio |
|---|---:|---:|---:|
| bs | 74.7 ± 1.6 | 103.1 ± 1.7 | 1.38 |
| 10dP | 9.3 ± 0.3 | 2.6 ± 0.1 | 0.28 |
| dm | 51.1 ± 1.4 | 139.3 ± 7.1 (recip 111.7) | 2.73 |
| T1 | 5.2 ± 1.4 | 5.9 ± 2.0 | — |
| dc | 82.5 ± 0.2 | 4.9 ± 0.8 | 0.06 |
| T2 | 2.1 ± 0.9 | 1.8 ± 0.9 | — |
| **total** | **291.2 ± 3.5** | **323.2 ± 9.8** | **1.11** |
| peak RSS | 248 GB | **351.5 GB** | 1.42 |

Per run: 324.4, 332.6, 312.3, 332.7, 313.9 s. The limb count is 7 % larger
(59.8 bits per limb): P, Q 2 222 222 226 limbs vs 2 076 205 062.

**Reading.** The switch does what WP2 intended — 10dP and dc fall from
92 s to 7 s — but the gain is eaten twice over: bs is 38 % slower
(seeds 20.6 vs 7.7 s, mdev 25 vs 12.5 s, batch 50 vs 46 s) and dm is
2.7× slower, almost all of it the reciprocal (112 vs 41 s; 67 mdev
products vs 48, 31 iterations vs 30). And the peak RSS rises from 248 to
351.5 GB — near the node's ceiling (375 GB, §45). Neither is explained by
7 % more limbs; the candidates are the decimal schoolbook (a `% 10¹⁸` per
column), the decimal CRT digit split (a Barrett step per Garner result on
every point of the mdev/batch tiers), and the tier thresholds, which are in
limbs and so shift with the base (67 vs 48 mdev products; the larger
peak is consistent with the top products landing in a heavier tier). These
are tuning items (WP4-style) if decimal is adopted; none was pursued, per
the rule that the user decides on the collected data.

**Net for the decision:** correctness ✓ at every size; single-node time
323 s vs 291 s (+11 %), peak 352 vs 248 GB; the multi-node reason for
decimal (no 10dP, no dc, digits are the limbs) stands, and the two
regressions have identifiable, un-attempted fixes.

## 51. Phase 7 WP5 — the distributed four-step transform (draft, 2026-09-16)

Code on `wp1-decimal-base` (to be moved to its own branch at the WP1
decision): `ecalc/ntt_dist.{h,c}` (plan, `dist_fwd/pw/inv`, and the
`_pre/_post` halves around the all-to-all for slab pipelining),
`ecalc/comm_sim4.c` (four synthetic ranks in one APU, driven by one thread),
`ecalc/tests/t_dist.c` (four-rank convolution vs the one-rank engine, all
four primes, R, C ∈ 2¹⁰..2¹³, plus 2²⁶). **`t_dist 26`: VERIFY OK (65
checks)** on s24-26 (2026-09-17) — the distributed transform, twiddles,
slab pack/unpack and the block-cyclic convention are right on the first
run. WP6's `comm_tcp` (`ecalc/comm_tcp.c`, TCP full mesh, pinned staging)
passes `t_comm` with 4 forked ranks on the node and 1–8 on littleblue;
`wp6run.sh` launches one process per APU across nodes (untested across
nodes: only one node is up today).

**Design as written.** Forward: local length-C pass on this rank's rows →
twiddle w_n^(i·j) (j read bit-reversed, as the local engine leaves it; two
tables of R and C entries, two `ec_mm` per point) → one slab all-to-all with
a local block transpose → local length-R pass on the columns. The result
stays in the column layout, so the pointwise product needs no transpose;
the inverse is the mirror (R⁻¹ and C⁻¹ split across the two local inverse
passes). One all-to-all per transform, three per product.

**Layout finding (a decision for the user).** Doing the local pass first
without an input permutation is only a DFT under the convention
*row i, column j ↔ point m = i + R·j* (F_n = P (F_R ⊗ I_C) D (I_R ⊗ F_C) S,
S = sort by m mod R). So a rank's rows are not one contiguous range of the
number's limbs but C runs of R/size contiguous limbs — block-cyclic
ownership with block R/size (e.g. R = 2¹⁷, 8 192 ranks → runs of 16 limbs).
Consequences: limb load/store is a local rows×C ↔ C×rows transpose (no
communication); carries and the CRT are limb-local except at run boundaries,
which need one neighbour exchange of C carry/propagate flags per product
(≈ 64 KB per rank, a parallel-prefix carry). The alternative — contiguous
limb ownership — makes the first pass the distributed one and costs two
all-to-alls per transform (≈ 25 s instead of ≈ 12.5 s of network per
4 × 10¹⁰ run on the target fabric). This is the "column-major four-step" note
from Phase 2, now with its cost quantified.

## 52. WP1 attribution, item 1 — where the decimal bs time goes (2026-09-17)

Measured on s24-26 (job 20630): `t_school` (CPU limb arithmetic, both
bases, node CPU, `tests/t_school.c`) and the 10¹⁰ run in both bases with
`RNS_VERBOSE=1` (`results/attr/e1e10_b{2,10}.log`), per-tier sums over the
bs phase.

**CPU schoolbook and mul_1 (ns per limb² / ns per limb, node CPU):**

| n | 2⁶⁴ school | 10¹⁸ school | ratio | 2⁶⁴ mul_1 | 10¹⁸ mul_1 | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 0.94 | 3.95 | 4.2 | 1.32 | 3.60 | 2.7 |
| 32 | 0.83 | 4.93 | 5.9 | 0.94 | 3.98 | 4.2 |
| 128 | 1.43 | 5.41 | 3.8 | 0.85 | 4.39 | 5.2 |
| 512 | 1.39 | 5.52 | 4.0 | 0.83 | 4.43 | 5.3 |
| 2048 | 1.37 | 5.55 | 4.1 | 0.82 | 4.46 | 5.4 |

Big passes at 2²⁶ limbs (ms): add 12.0/12.0, sub 12.3/13.0, cmp 0/0,
shr_limbs 5.8/4.4, mul_1 4.8/14.3 — add, sub, cmp and shifts are
base-independent; only the multiply-by-word costs more. (On littleblue the
ratios are 12–20×: its compiler emits `__umodti3` for the 128-bit `% 10¹⁸`.)
**Cause:** `limb_mul_school` and `mul1_serial` reduce every inner product
with a 128-bit division by 10¹⁸ (`bigint.c`); a product a·b < 10³⁶ ≈ 2¹¹⁹·⁶
leaves room for ≈ 2⁸ products in a u128 accumulator, so a lazy reduction
(reduce once per 256 products, or one Barrett step per column) would remove
essentially all of the 4–5×. This is the seeds' 20.6 vs 7.7 s at 4 × 10¹⁰.

**GPU batch tier, bs phase at 10¹⁰ (22 levels, s):**

| | scatter | ntt | crt | merge | total |
|---|---:|---:|---:|---:|---:|
| 2⁶⁴ | 2.56 | 3.65 | 1.78 | 0.09 | 7.64 |
| 10¹⁸ | 1.79 | **5.60** | 1.89 | 0.14 | 9.54 |

The decimal CRT digit split costs +6 % (crt 1.78 → 1.89): cheap. The +53 %
is the transform length: **at every one of the 20 levels the decimal
products use a 2× longer NTT** (L = 2¹¹ where binary used 2¹⁰, … 2³⁰ vs
2²⁹), because the 7 % larger limb count pushes each level's product size
from just under a power of two to just over it (the levels' sizes double,
so one crossing repeats at every level). This is an alignment accident of
the term count, not a property of the base: it is removed by choosing the
seed span so the decimal node sizes land under the powers of two again
(e.g. `bs_seed_terms` 512 → 480 in decimal), or by 3·2ᵏ lengths (WP8).
The same crossing is the likely cause of bs's mdev 25 vs 12.5 s at
4 × 10¹⁰ (the top levels; to be confirmed with `RNS_VERBOSE` at that size).

**Lead for items 2–3 (from the same logs):** the reciprocal's Newton
sequence is identical in both bases (40 mdev products, 2¹¹ … 2³⁰) **plus one
extra doubling in decimal** (2 × 2³¹ products), because the required
precision k = 5.56 × 10⁸ limbs crosses 2²⁹ while binary's 5.19 × 10⁸ does
not; that iteration is the whole difference (mdev 5.8 vs 3.5 s, recip 17.8
vs 10.7 s). At 4 × 10¹⁰ the decimal k = 2.22 × 10⁹ crosses 2³¹ (binary
2.08 × 10⁹ does not), which would explain both the reciprocal's 112 vs 41 s
and the 352 GB peak (one more doubling with 2³²-point products and their
temporaries). The last doubling is computed at full 2j precision even when
k is only slightly above j; truncating it to k (`take = k + 2`) would make
the final step cost k, not 2j, in both bases. Item 2 verifies this at
4 × 10¹⁰.

## 53. WP1 attribution, items 2–3 — the reciprocal and the 352 GB peak (2026-09-17)

4 × 10¹⁰ in both bases with `NEWTON_VERBOSE=1` (per iteration: j → j′,
product size, r length, VmRSS/VmHWM) and `RNS_VERBOSE=1`
(`results/attr/e4e10_b{2,10}.log`, job 20630, s24-26). Whole-run totals
this pass: binary 286.6 s / 248.3 GB, decimal 328.5 s / 351.5 GB.

**The Newton sequences are identical through j = 2³⁰** (30 doublings, RSS
within 0.5 GB of each other after subtracting the bs-phase offset). Then:

| | binary (k = 2 076 205 063) | decimal (k = 2 222 222 226) |
|---|---|---|
| step from 2³⁰ | → k directly (×1.93): take 2.08 × 10⁹, r 2³¹ + 1 limbs, RSS 231.7 GB | → 2³¹ (full doubling): r 2³¹ + 1, RSS 254.5 GB |
| step from 2³¹ | — | → k (×1.035): take 2.22 × 10⁹, **r 2³² + 1 limbs, RSS 333.8 GB** |
| recip | 41.0 s, 48 mdev | 116.0 s, 67 mdev |
| peak (recip) | 248.3 GB | 351.5 GB |

Because k is 3.5 % above 2³¹ in decimal and 3.3 % below it in binary,
decimal pays one extra doubling, and the last step — needed for only 3.5 %
more precision — is computed as a full step from j = 2³¹: products of
2³²–2³³ points and an r₂ of 2³² + 1 limbs (34 GB). That step is the
reciprocal's +75 s and the peak's +100 GB. **Item 3 needs no further
measurement: the 352 GB is this iteration** (333.8 GB RSS inside it, 351.5
at its products), exactly as the binary 248 GB peak is its own last step
(231.7 → 248.3).

The same power-of-two crossing appears twice more: the top bs level
(P·Q = 4.44 × 10⁹ limbs > 2³² vs binary 4.15 × 10⁹ < 2³²: mdev 25.6 vs
12.7 s, bs-phase HWM 227 vs 184 GB) and dm's A·μ (18 mdev vs 6; division
after recip 26 vs 11 s). Together with §52 (seeds, batch levels) every
decimal regression at 4 × 10¹⁰ is now attributed; none is a per-limb cost
of base 10¹⁸ except the CPU schoolbook's division.

**Fixes identified (not applied — the user's decision):**
1. *Anchor the doubling sequence at k* (j_i = ⌈k/2ⁱ⌉ instead of powers of
   two): the last step then goes 1.11 × 10⁹ → 2.22 × 10⁹ with 2³²-point
   products, the same size as binary's last step; the extra iteration
   becomes a tiny one at the start. Expected: recip ≈ 41 × 1.07 ≈ 44 s, peak
   ≈ 248 × 1.07 ≈ 265 GB. Small change in `newton_recip_seeded`; benefits
   binary too (every level slightly smaller). Removes ~72 s and ~85 GB.
2. *Lazy reduction in the decimal schoolbook/mul_1* (§52): seeds 21.7 → ≈ 9 s.
3. *The genuine crossings* (top bs level, A·μ): ~+13 s in bs and ~+15 s in
   dm remain unless transform lengths 3·2ᵏ exist (WP8: 2³³ → 3·2³¹, 0.75×)
   or the top level is split (Karatsuba: same work, half the memory).

**Projection (item 4, from these measurements, not a measurement):**
decimal with fixes 1 + 2: bs ≈ 95, 10dP 2.5, dm ≈ 70, T1 + T2 ≈ 6, dc 4.4 →
**≈ 178 s and ≈ 265–275 GB peak, vs binary 287 s / 248 GB**; with WP8's
3·2ᵏ lengths as well ≈ 165 s. Fix 1 alone brings decimal to ≈ 250 s / 265 GB.

## 54. WP1 assessment — measured with the two experimental fixes (2026-09-17, job 20638, s24-26)

Fix 1: k-anchored Newton doubling (`NEWTON_ANCHOR=1` default; 0 restores
the Phase 4 sequence). Fix 2: decimal schoolbook as column sums reduced
once per column (node: 0.98–1.2 ns/limb² at n ≥ 128, was 4–5.5; binary
1.4). A double-quotient `mul_1` was tried and dropped (slower than the
compiler's constant division on the node: 7.2 vs 4.4 ns/limb).

Correctness with the fixes: `t_mul` 133/135, `t_newton` 658/658, `t_bs`
10/10 in binary/decimal; **e to 10⁹ byte-identical to the reference in both
bases**; 4 × 10¹⁰ VERIFY OK in both (T1, T2, digit residues).

| 4 × 10¹⁰, one run each | binary before (mean of 5) | binary + fix 1 | decimal before (mean of 5) | decimal + fixes |
|---|---:|---:|---:|---:|
| bs | 74.7 | 72.3 | 103.1 | 107.1 (seeds 21.1, batch 50.1, mdev 26.5) |
| 10dP | 9.3 | 9.1 | 2.6 | 2.8 |
| dm (recip) | 51.1 (41.0) | **44.2 (33.1)** | 139.3 (111.7) | **72.2 (45.2)** |
| T1 + T2 | 7.3 | 5.4 | 7.7 | 6.3 |
| dc | 82.5 | 78.1 | 4.9 | 4.8 |
| **total** | **291.2 ± 3.5** | **271.0** | **323.2 ± 9.8** | **259.0** |
| peak RSS | 248.3 | 246.3 | 351.5 | 293.5 |

Newton trace with anchoring: binary 30 iterations ending
521 187 878 → 1 042 375 754 → k; decimal 31 ending 1 111 111 113 → k with
products of 2³² points (r 2 222 222 227 limbs, RSS 275.7 GB inside the
step) — the 2³³ step of §53 is gone.

**Reading.**
- Fix 1 is a **binary improvement too**: 291 → 271 s (−7 %, recip 41 → 33 s)
  with identical digits; it is base-independent and can be taken on `main`
  regardless of the WP1 decision (one run; the Phase 4 variance is 1.2 %).
- Decimal is now **faster than binary on one node: 259 vs 271 s**, with the
  base's structural gain (10dP + dc: 92 → 8 s) no longer cancelled. Its
  peak is 294 GB vs 246 — 47 GB more, 80 GB under the node ceiling.
- What remains decimal-specific: the seeds (21.1 vs 7.3 s) — fix 2 did not
  touch them because their cost is `mul_1` in the span loop (4.4 vs 0.8
  ns/limb, the 128-bit division by 10¹⁸ per limb); a faster constant
  division (Barrett on the split words) is the un-attempted fix, ≈ 10 s.
  The top-level crossing (bs mdev 26.5 vs 11.5, dm's A·μ 27 vs 11 s,
  bs-phase peak 227 vs 184 GB) is the 2³³-point products of 4.44 × 10⁹-limb
  operands; only 3·2ᵏ lengths (WP8) or a top-level split remove it
  (≈ 25 s, ≈ 40 GB).

## 55. WP3 go/no-go — where the pools should live (2026-09-17, job 20638, s24-26)

`bench/fabric/poolread` (32 GiB pool, 4 APUs reading in the batch scatter's
pattern) and `bench/fabric/cpuhbm` (4 GiB per allocation kind: CPU
read/write from the local and a remote NUMA node, GPU read from the local
and a remote APU); `results/attr/wp3.out`.

| pool layout | aggregate GPU read |
|---|---:|
| (a) today: registered host pages spread over the four nodes | 553 GB/s (APU0–2 ≈ 66–99, APU3 321) |
| (b) quarters in HBM, every APU reads everything | 702 |
| (e) NUMA-local host quarters, every APU reads everything | 704 |
| (c) replicated in every HBM (4 × memory) | 13 300 |
| **(d) NUMA-local quarters, each APU reads its own quarter** | **27 700** (cache-assisted; single-read rate 3.8 TB/s per APU → ≈ 15 000) |

| allocation on APU 0 (XNACK off) | CPU node 0 rd / wr | CPU node 3 rd / wr | GPU 0 rd | GPU 3 rd |
|---|---:|---:|---:|---:|
| malloc, touched by node 0, registered | 131 / 105 | 24 / 24 | 3 774 | 94 |
| hipHostMalloc | 152 / 126 | 24 / 24 | 3 739 | 93 |
| hipMalloc (coarse) | **164 / 114** | 25 / 24 | 3 769 | 94 |
| hipExtMalloc fine-grained | 153 / 114 | 24 / 24 | 3 756 | 93 |

Facts: (1) on MI300A a GPU reads any memory on its own node at ≈ 3.8 TB/s
— hipMalloc'd or OS pages alike — and any remote node's at ≈ 93 GB/s; the
allocation kind is irrelevant, the node is everything. (2) So every layout
in which each APU reads the whole pool is capped at ≈ 700 GB/s by the
links; the gain (≥ 20×) comes only from **locality: a product transformed
by the APU whose node holds its operands**. (3) The CPU reads/writes
hipMalloc'd memory on its own node at full rate with XNACK off (164/114
GB/s), so device pools can hold the numbers for the CPU passes too.
(4) `HSA_XNACK=1` doubles the pipeline time (10¹⁰: 80 → 159 s): unified
paging is out; registered host memory or hipMalloc, both with XNACK off.

**Go.** WP3 layout: the binary-splitting level pools live in four
per-APU device pools (hipMalloc, CPU-accessible), each APU owning a subtree
of the tree (the same partition WP5 uses for ranks), so a level's products
are transformed where their operands are, with no operand staging copy and
the CRT writing results back in place; only the top two levels' products
cross APUs (they are mdev-tier stripes already). The dm-phase numbers
(A, Q, μ, X) and their mdev-tier products move to quartered device pools
with the 4-APU distributed transform — that is WP5's `ntt_dist` on real
APUs, so it is done there, on this layout. Device-pool bytes do not appear
in RSS: the memory accounting will report host RSS + device pools.

## 56. WP3 — device-resident level pools and the locality-aware batch tier (2026-09-17/18, s24-26 and s24-16)

**What was built** (branch `wp1-decimal-base`, all on top of WP1 + the
k-anchored Newton):
- `mem`: device pools the CPU can also use (`mem_dev_alloc`, hipMalloc, XNACK
  off), `mem_dev_of`, DMA copies (`mem_dev_copy`), optional thread pinning.
- `binsplit`: the level pools are four device regions with subtree ownership
  (node i of a level of n → region ⌊4i/n⌋); seeds computed into the pinned
  NUMA-local `hstage` buffers and DMA'd in; the mdev-tier levels (top three)
  keep their results in a host pool pregrown at init (a single region can need
  the whole level there; WP5 replaces this tier); the top level is computed
  straight into P and Q; all pools pregrown at init.
- `rns_mul_batch`: the **locality-aware path** — APU d computes all four
  primes of the products in its own region (`k_scatter4`, four contexts per
  device, CRT on its own four planes, results in place), no staging, no peer
  traffic except boundary pairs; the striped prime-per-device path remains for
  host-resident or very few/huge products (`RNS_BATCH_LOCAL_MIN`).
- The binary-splitting add P = P₁Q₂ + P₂ is folded into the CRT kernel
  (`rns_prod.x`); normalised lengths come back from a kernel (`k_norm`).
  No CPU pass touches the pools in the batch levels.
- `e_terms` by bisection: the linear `lgamma` scan cost ≈ 45 s at 4 × 10¹⁰ and
  had been inside the wall time since Phase 3 (outside every phase timer).
- The run now prints `phases`, `init` and `other` separately.

**What was learned on the way** (each a measured dead end, kept out):
CPU read-modify-write and single-limb reads of device memory are
latency-bound (add+norm 3.0 → 10 s; fixed by moving both to the device);
CPU streaming stores into device pools run at ≈ 8 GB/s (fixed by DMA);
hipMalloc after peer access is enabled costs ≈ 0.06 s/GB (pools pregrown at
init); pinning threads to their node costs the decimal seeds 30 % and is now
unnecessary (off by default).

**Batch tier, per level (10¹⁰, binary, level 1):** scatter 0.141 → 0.024 s,
crt 0.077 → 0.017 s, GPU part 0.44 → 0.18 s. Whole batch tier at 10¹⁰:
11.2 → 6.9 s (binary), 12.9 → 9.9 s (decimal); bs 13.7 → 10.6 / 21.0 → 18.9.

**4 × 10¹⁰, one run each, VERIFY OK, 10⁹ byte-identical in both bases**
(job 20642, s24-16; "before" = §54, the same code without WP3):

| | binary before | binary WP3 | decimal before | decimal WP3 |
|---|---:|---:|---:|---:|
| bs | 72.3 | **47.7** (seeds 10.3, batch 27.5, mdev 9.8) | 107.1 | **75.2** (seeds 21.8, batch 30.4, mdev 22.9) |
| 10dP | 9.1 | 9.3 | 2.8 | 3.1 |
| dm (recip) | 44.2 (33.1) | 41.1 (29.7) | 72.2 (45.2) | 67.8 (41.3) |
| T1 + T2 | 5.4 | 5.5 | 6.3 | 5.9 |
| dc | 78.1 | 82.7 | 4.8 | 4.2 |
| **phases** | **209.1** | **186.5** | **193.2** | **156.2** |
| init / other | 11.5 / 50 | 19.5 / 7.9 | 12 / 54 | 20.4 / 7.8 |
| **wall** | 271.0 | **213.9** | 259.0 | **184.4** |
| peak RSS | 246 | 246 | 294 | 293 |
| device pools | 128 | 119 (bs) / 128 | 128 | 116 / 128 |

The batch tier is 1.6× faster (44.8 → 27.5 s binary), the mdev levels
are unchanged within noise (they still stage through the host; WP5), and
the wall time drops a further ≈ 45 s in both bases from the `e_terms`
fix. Peak memory is set by dm and unchanged. Decimal seeds 21.8 s
(unpinned; 28 s pinned) remain the WP4 item.

## 57. WP8 — transform lengths 3·2ᵏ (2026-09-17, s24-16, job 20642)

**Built.** (1) A prime set with 3·2⁴⁴ ∣ p−1 (`EC_PRIMES=1`, default; the
Phase 3–7 set stays buildable with `EC_PRIMES=0`): 4 222 124 650 659 841,
3 799 912 185 593 857, 3 641 582 511 194 113, 2 586 051 348 529 153
(c = 240, 216, 207, 147 in c·2⁴⁴ + 1; two are the old p₁, p₃; all < 2⁵²,
product 2²⁰⁶·⁶), with primitive 3·2³³-th roots (`ec_root3`), checked by
`t_modarith` against GMP. Every test and the 10⁹ digits are unchanged with
the new set at 2ᵏ lengths. (2) `ntt3.c`: length 3·2ᵏ as a DIF radix-3
stage over the thirds followed by three 2ᵏ transforms through the existing
engine (batch 3), the mirrored inverse, two-level twiddle tables per
(device, prime, k); `t_ntt3` (identity and convolution against the 2ᵏ⁺²
engine, all primes, k = 10..24): 120 checks. (3) Length choice in the
mdev and locality-aware batch tiers: 3·2ᵏ⁻² whenever nc ≤ 3·2ᵏ⁻² (0.75×
the points); stride-aware scatter and CRT; `RNS_R3=0` disables it.

**Correctness.** `t_mul` 133 / big 137 / batch 16 (against GMP, the mdev
tier now on 3·2ᵏ), `t_crt` 24, `t_bs` and `t_newton` in both bases, e to
10⁹ byte-identical in both bases with 3·2ᵏ transforms in use, 4 × 10¹⁰
VERIFY OK in both bases.

**4 × 10¹⁰ (one run each; WP3 = §56):**

| | binary WP3 | binary WP8 | decimal WP3 | decimal WP8 |
|---|---:|---:|---:|---:|
| bs | 47.7 | 48.2 (mdev 9.8 → 11.3, noise) | 75.2 | 71.6 (mdev 22.9 → 21.7) |
| dm (recip) | 41.1 (29.7) | 42.0 (30.0) | 67.8 (41.3) | 64.0 (38.9) |
| **phases** | 186.5 | 185.9 | 156.2 | **149.2** |
| peak RSS | 246 | 246 | 293 | 293 |

Where the lengths land: decimal's Newton products run as 35 × 3·2²⁹
(1.11 × 10⁹-limb halves, 0.96 s each) instead of 2³¹; binary's as 18 × 2³¹
(nothing of binary's sits just above a power of two at this d).

**Correction to §53's projection (−25 s / −40 GB).** The top-level
products (4.4 × 10⁹ limbs) exceed the 2³¹-point mdev pool in both bases
and go through the Karatsuba-split tier; only their halves get the 0.75×,
and the 293 GB peak is that tier's host temporaries (sized in limbs), not
a transform. The measured value of WP8 is −7 s in decimal, 0 in binary,
and 0 in memory at this digit count. Its structural value stands: both
bases are now insensitive to where d falls against powers of two (the
crossing that cost decimal 2.7× in dm before the Newton anchoring cannot
recur), which matters for the multi-node digit counts.

## 58. WP4 — compute tuning on the final layout (2026-09-17, s24-16, job 20642)

Three items, all measured at 4 × 10¹⁰, both bases verified after each:

1. **Decimal `mul_1`** (the seed spans multiply by term indices < 2³³): a
   64-bit Barrett step (μ = ⌊2⁹⁴/10¹⁸⌋, one multiply-high, ≤ 1 correction;
   exact on 2 × 10⁸ random inputs) replaces the 128-bit division. Node:
   4.4 → **2.6 ns/limb** (binary 0.82). Decimal seeds 22.4 → 16.3 s.
2. **CPU schoolbook tier off** (`bs_school_nl` 160 → 0): with the pools
   in device memory the tier's read-modify-write costs 42–59 s at a level
   the device batch tier does in 1.4–2.5 s; it had been dormant at S = 512
   (level-0 nodes are larger than its threshold) and appeared the moment
   the seed span shrank.
3. **Seed span** S (`BS_SEED_TERMS`): seed work is ∝ N·S, each halving
   adds one small batch level.

| S | binary seeds / bs | decimal seeds / bs |
|---|---:|---:|
| 512 (old) | 9.9 / 50.4 | 15.8 / 65.0 |
| **256 (new default)** | **6.4 / 44.2** | **9.7 / 61.4** |
| 128 | 5.1 / 53.6 | 5.8 / 57.7 |

**Full runs, S = 256** (one each): decimal bs 62.1, dm 62.1 (recip 39.4),
**phases 138.1 s**, wall 166.7, peak 293.5 GB; binary bs 47.5, dm 42.7,
dc 80.1, **phases 187.1 s**, wall 214.7, peak 246.3 GB. Digits verified
(T1, T2, residues) in both.

**Not done, sized:** decimal's split halves (2.22 × 10⁹ limbs) do not fit
the 2³¹-point mdev pool where binary's 2.08 × 10⁹ do, so decimal splits
one level deeper (35 × 3·2²⁹ vs 18 × 2³¹ calls) — ≈ 10 s in bs's mdev
levels and ≈ 10 s in dm's division. Fitting them needs 3·2³⁰-point planes
(51.5 GB per device, 206 GB of pools), which with the host copies of the
dm numbers exceeds the node (RSS 293 + 206 GB); it becomes possible when
WP5 makes those numbers device-resident.

**Where the bases stand after WP1–WP4 + WP8 (4 × 10¹⁰, one node):**
decimal 138 s of phases and 294 GB against binary 187 s and 246 GB; the
decimal-specific per-limb costs left are the seeds (9.4 vs 5.4 s) and the
deeper split above.

## 59. WP5 (in progress) — the distributed transform on real APUs, and the ownership question settled (2026-09-17)

- `comm_xgmi`: four real ranks (one per APU, peer copies on the rank's
  stream, a shared barrier), driven by four host threads. **`t_dist` on four
  real APUs: VERIFY OK (66 checks, up to 2³⁰ points)** — the distributed
  four-step transform, twiddles, slab transposes and the block-cyclic
  convention are right on real hardware.
- **Ownership settled.** §51 asked whether contiguous limb ownership could
  avoid the second all-to-all per transform. I tried a "transposed
  inverse" (the same algorithm as the forward run on the column layout);
  modelled on a 16-point case and run on the node it ends in the same
  block-cyclic layout — the last local pass's row index is always the
  residue mod R, whichever factorisation is used. So: block-cyclic
  ownership = 3 all-to-alls per product; contiguous ownership = the same 3
  plus a redistribution in and out (on one node a peer gather/scatter, on
  the fabric two more all-to-all-sized exchanges). The single-node product
  tier below keeps the numbers contiguous at its interfaces (what every
  limb operation wants) and is block-cyclic inside.
- `rns_mul_dist` (the tier): gather the operands' block-cyclic rows from
  contiguous memory (coalesced column runs), forward transforms and the
  inverse over the four APUs (3 all-to-alls), a local transpose so each
  column run is contiguous, CRT with one stripe per run into a local
  buffer, runs scattered to their limb positions, run spills merged on the
  host in limb order. **Correct** (GMP-checked products from 1.1 × 10⁶ to
  2.7 × 10⁸ limbs in both bases, equal to `rns_mul`'s at every size).
- **Time at 2³¹ points (four primes, 2 forward + 1 inverse each):** the
  transform parts per rank after tuning — local passes 0.33 s, fused
  LDS-tiled twiddle+pack/unpack 0.12 s (was 0.36 in three passes),
  all-to-all 0.18 s (was 0.45 with `hipMemcpyPeerAsync` and with three push
  kernels on separate streams; one kernel serving the three links at once
  runs them concurrently), CRT 0.08 s — **0.75 s against the mdev tier's
  1.30 s** for the same product. The tier's wall time (4.7 s) is host
  staging: `memcpy` of 16 GB operands in and the result out. That is what
  step 3 (device-resident numbers) removes.
- **Step 3 — device-resident numbers (correct, 2026-09-17).** `dbig`: a big
  integer in four quarters (one per APU, power-of-two quarter size, views with
  an offset), every operation one kernel per APU over its quarter with chunk
  carry flags scanned on the host (add, sub, limb shifts, compare, normalise,
  B^k); `t_dbig` against the host bigint: 505 checks in each base. Two
  platform facts found on the way: concurrent `hipMemcpy` with pageable host
  memory from several threads faults (the phase-boundary copies are serial);
  concurrent peer reads in all directions are fine (`bench/fabric/peerconc`).
  `rns_mul_dist_db`: the tier on device operands through limb accessors,
  one B plane at a time so 2³¹ points fit the standard pools, products above
  that split in halves on device, the run spills added as a sparse temporary
  with a chunked-carry `db_add` (an atomic ripple was pathological on long
  carry chains). `newton_db`: the reciprocal and division mirrored on `dbig`
  (same anchored doubling, corrections and statistics), `NEWTON_DEVICE=1`.
  **`t_newton` 658 checks in both bases; e to 10⁹ byte-identical in both
  bases with the Newton phase on device.** Timing: pending the allocator fix
  (fresh `hipMalloc` per temporary dominated: 1 s per 17 GB number).
- **4 × 10¹⁰ binary with the dm phase on device — VERIFY OK (2026-09-18):**
  reciprocal **15.1 s vs 29.7 s** on the host path (the products 7.3 s, the
  limb passes 3.1 s, allocation 3.7 s), peak RSS 224 GB (host path 246), but
  the division 26 s vs 11.5 (low-product recursion splitting twice, two
  17 GB host round trips of μ, a 34 GB temporary allocated). Fixes applied
  (views for the shifted operands, swaps for the iterate and μ, μ kept on
  device, full-then-truncate low product, a 3·2ˡ quarter class so decimal's
  4.44 × 10⁹-limb temporaries take 51 GB instead of 68); re-measurement
  running in both bases.

## 60. Design choices, as measured (draft, 2026-09-18; final numbers at the end of the session)

Every choice below was decided on a measurement in this record; the section
lists the choice, the alternative, the number that decided it, and where
the number is.

| choice | alternative | measured | where |
|---|---|---|---|
| FP64 Barrett modmul (engine 1, 4 × 52-bit primes) | Shoup / Montgomery, 62-bit primes (engine 2) | Shoup 2–3 % slower in a real pass; engine 2 2.2× slower end to end | §44, §47 |
| register-blocked 7-stage b16 body | the paper's tile kernel | +19 % / +15 % at 2³¹, bit-identical | §43 |
| striped GPU CRT into registered host memory | 192-thread CPU Garner | 0.5 s vs 0.8–1.5 s per 2³¹; batch tier 1.2 → 4.3 Gpt/s | §39, §43 |
| correction-form Newton, k-anchored doubling | the paper's truncated step; power-of-two doubling | anchoring: 291 → 271 s binary, decimal recip 112 → 45 s | §53, §54 |
| decimal limbs (base 10¹⁸) as the pipeline's base | binary limbs + 10dP + radix conversion | 10dP + dc 92 → 8 s; bs +38 % (seeds, since tuned), peak +47 GB; multi-node: no distributed dc | §50–54, §58 |
| level pools as four device regions, subtree ownership, locality-aware batch tier | prime-per-device stripes from spread host pools | own-node reads 3.8 TB/s vs 93 GB/s remote; batch tier 44.8 → 27.5 s | §55, §56 |
| the CPU kept out of device pools (adds/normalisation in the CRT kernel, DMA copies) | CPU passes over device memory | RMW/latency-bound passes 3–20× slower; CPU streaming stores 8 GB/s | §56 |
| pools pregrown at init; bs regions donated to the dm phase | allocate per phase | hipMalloc 0.057 s/GB (flat), 0.02 s/GB to free | §59 |
| 3·2ᵏ transform lengths, prime set with 3·2⁴⁴ ∣ p−1 | 2ᵏ only | removes the power-of-two crossings; decimal −7 s at 4 × 10¹⁰, no memory change (the top products are split) | §57 |
| seed span 256, no CPU schoolbook tier, Barrett mul_1 for the decimal seeds | 512, school tier at ≤ 160 limbs | seeds 9.9 → 6.4 s (binary), 15.8 → 9.7 (decimal); the school tier cost 42–59 s once the pools were device memory | §58 |
| block-cyclic limb ownership inside the transform (3 all-to-alls per product), contiguous numbers at the interfaces | contiguous ownership throughout | no single-all-to-all inverse exists (modelled and run); 3 + gather/scatter vs 6 | §51, §59 |
| all-to-all as one push kernel over the three links | hipMemcpyPeerAsync; one kernel per link on separate streams | 0.45 → 0.18 s per 2³¹ product's 12 exchanges | §59 |
| dm phase on device-resident numbers (dbig + distributed tier) | host-resident numbers, prime-per-device mdev with staging copies | reciprocal 29.7 → 12.0 s at 4 × 10¹⁰; division on par (the two split products dominate) | §59 |
| dbig carries by chunk flags + a parallel-prefix kernel; spills as a sparse operand | one thread per chunk; atomic ripple | latency-bound at ~100 GB/s; pathological on long carry chains | §59 |
| top bs levels as device numbers through the distributed tier, region pools donated | host mdev tier with staging copies | levels 23–24 3× faster; peak host RSS 233 → 160 GB (decimal); CPU-phase variance gone | §64, §62b |
| device products over one plane split as a cost-minimising grid of pieces | halving the longer operand recursively | decimal 2.2e9² products 8 → 6 planes; dm 48.9 → 38.0 s, phases 119 → 108 s; no memory change; the same work a 3·2³⁰ plane pool would give | §66 |

Open trade-offs (sized, not chosen): a 3·2³⁰-point plane pool for the
dist tier (+60 GB of device pools; after §66 it would only lift the fill of
the 2.2e9² products from 86 % to 69 %-of-a-larger-plane — no longer worth
it); Karatsuba instead of the grid in the device tier: for binary's 2.07e9²
products 3 planes instead of 4 (≈ −2.5 s of dm), but decimal's half-sums
(1.11 + 1.11 × 10⁹ limbs) do not fit a plane, so it does not apply where
the split costs most — untested.
- **4 × 10¹⁰ with the dm phase on device, both bases — VERIFY OK
  (2026-09-18, job 20644, s24-16, one run each, final WP5 code):**

| | binary host-resident dm (§58) | binary device dm | decimal host-resident dm (§58) | decimal device dm |
|---|---:|---:|---:|---:|
| recip | 31.8 | **12.4** | 39.4 | **23.3** |
| division | 10.9 | 19.7 (A μ 9.5, low product 7.4, copies 0.7) | 22.7 | 32.7 (A μ 16.2, low product 13.1) |
| dm | 42.7 | **32.1** | 62.1 | **56.0** |
| **phases** | 187.1 | **175.0** | 138.1 | **130.5** |
| host RSS in dm | 155 | 79 | 161 | 82 |

  What made the difference: no host staging of the Newton temporaries
  (bounce-buffer copies only at the phase boundary: 0.7 s), the products
  through the four-APU distributed transform (§59 tier: 1.2 s per 2³¹-point
  product against mdev's 1.3), the bs regions donated to the dm phase's
  block allocator (no hipMalloc inside the loop except a few GB), views and
  swaps instead of temporaries. What remains: the two products of
  4.15–4.44 × 10⁹ limbs (A μ and X Q) each split into four 2³¹-point tier
  calls (≈ 5 s per product in binary, 8 in decimal) — a 3·2³⁰-point plane
  pool (+40 GB device) or Karatsuba would take ~30–40 % off them (§60).

## 61. WP7 — checkpoint and restart of the binary-splitting phase (2026-09-18; details in `results/WP7.md`)

Implemented by an agent in an isolated worktree, merged into the branch.
Per-level snapshots of the bs level loop (node table + the used limbs of
the four region pools, or of the mdev levels' host pool), every
`BS_CKPT_EVERY` levels from level 8 or 1 GiB of pool, written to temporary
names and renamed with the header last (a complete set always exists);
`BS_RESTART=1` resumes from the latest complete set, skipping the seeds and
the lower levels. No numerical code touched; no streaming of the working
set (periodic snapshots only).

**Verified (job 20644, s24-16):** at 10⁸ and 10⁹ in both bases, four runs
each — checkpointing, restart from its last set, a run killed right after a
checkpoint, restart from that — all VERIFY OK and **digits `cmp`-identical
in all 15 comparisons** (against each other and the reference files).

**Cost:** 0.83 GB (binary) / 0.89 GB (decimal) per checkpoint at 10⁹,
0.5–0.9 s each (NVMe write + fsync ≈ 1 GB/s; the DMA side 1.6–2 GB/s);
restart load 0.13–0.22 s. Extrapolated: ≈ 9 GB / 9 s at 10¹⁰, ≈ 35 GB /
35 s at 4 × 10¹⁰ — every 4 levels of a 24-level tree, i.e. ≈ 4 snapshots.
**10¹⁰ (binary, run by the main session on the merged code):** five
snapshots of 8.3 GB at levels 4–20 (the 1 GiB threshold fired from level
4), 5.7 s each (1.3–2.1 GB/s), restart from level 20 loaded in 1.2 s,
**digits identical** — this exercises the mdev-level host-pool path. The
cost showed the default policy was too eager at scale (28 s of snapshots
over a 12-s bs phase; ≈ 175 s at 4 × 10¹⁰): the defaults are now level ≥
16 (or a 64 GiB pool), i.e. two snapshots of a 24-level run, ≈ 70 s at
4 × 10¹⁰ — the top levels are where a restart saves time.
For the multi-node run the same snapshot per rank, at the level boundaries
that are global synchronisation points anyway, is the restart design.

## 62. Five-run variance on the final single-node code (2026-09-18, job 20644, s24-16; `NEWTON_DEVICE=1`, seed span 256, 3·2ᵏ lengths)

| 4 × 10¹⁰ | binary mean ± sd | decimal mean ± sd |
|---|---:|---:|
| bs | 46.0 ± 1.6 | 63.1 ± 3.0 |
| 10dP | 10.7 ± 1.5 | 5.1 ± 2.8 |
| dm | 31.7 ± 0.3 | 56.6 ± 1.1 |
| T1 | 4.1 ± 0.3 | 6.8 ± 2.3 |
| dc | 80.7 ± 0.9 | 5.5 ± 0.9 |
| T2 | 1.3 ± 0.1 | 2.6 ± 1.2 |
| **phases** | **174.5** (173.2–175.2) | **139.6** (129.6–150.3) |
| init / other | 20.7 / 14.2 | 20.8 / 10.0 |
| **wall** | **209.4 ± 1.6 (0.8 %)** | **170.3 ± 11.7 (6.9 %)** |
| peak RSS | 232.9 GB | 232.7 GB |

All ten runs VERIFY OK. The GPU-heavy phases are stable (dm 1–2 %, dc
1 %); decimal's spread is entirely in the CPU-side phases (10dP 3.5–10 s,
T1 4.6–9.9, T2 1.2–3.5, "other" 6.9–13.4) — the same code in binary varies
less because those phases are shorter relative to the rest. Not thread
placement: at 10¹⁰ the same phases are stable to 0.1 s with no binding,
`OMP_PROC_BIND=spread` and `close` alike (`results/bind.out`). The spread
appears only with ≈ 230 GB of resident host memory: the decimal 10dP is a
limb shift into a fresh 35 GB allocation, and its time is first-touch page
faults (4–10 GB/s). Huge pages were tried (`MADV_HUGEPAGE`, the node's
THP mode is `madvise` with `defer+madvise` defrag): **worse** — 10dP
26 s, bs +6 s, peak RSS +17 GB from compaction stalls; kept behind
`BI_HUGE=1`, off. **Fixed by reusing a pre-faulted buffer:** in decimal, A takes the bs
phase's host pool (≈ 40 GB, already faulted) instead of a fresh allocation
— 10dP 6.9 → **2.2 s** at 4 × 10¹⁰ (job 20644, digits verified at 10⁹ and
4 × 10¹⁰); the same page-fault cost is what makes "other" and T1 vary. Best observed
decimal run: 129.6 s of phases, 157.0 s wall.

## 62b. Five-run variance, everything on device (2026-09-18, job 20655, s24-16; `BS_DEV_MDEV=1 NEWTON_DEVICE=1`, `results/variance_b{2,10}_dev2/`)

Same as §62 plus the top bs levels on the device tier (§64, now the
default).

| 4 × 10¹⁰ | binary mean ± sd | decimal mean ± sd |
|---|---:|---:|
| bs | 42.4 ± 0.7 | 59.6 ± 1.2 |
| 10dP | 8.9 ± 0.3 | 1.6 ± 0.3 |
| dm | 28.3 ± 0.1 | 48.9 ± 0.3 |
| T1 | 4.2 ± 0.2 | 3.7 ± 0.1 |
| dc | 80.9 ± 0.8 | 4.2 ± 0.1 |
| T2 | 1.9 ± 1.0 | 1.3 ± 0.1 |
| **phases** | **166.6** (165.1–168.8) | **119.2** (117.4–121.3) |
| init / other | 19.4 / 3.1 | 19.8 / 3.1 |
| **wall** | **189.1 ± 1.6 (0.8 %)** | **142.0 ± 1.8 (1.3 %)** |
| peak RSS | 232.8 GB | 160.0 GB |

All ten runs VERIFY OK. The decimal spread of §62 (6.9 % on wall) is
gone: the CPU-side phases that varied — 10dP, T1, T2, "other" — all lost
their page-fault cost once the bs host pools (≈ 72 GB) stopped existing;
"other" fell from 10 s to 3 s in both bases for the same reason. Every
phase is now within 2.5 % run to run except binary's T2 (one 3.7 s
outlier in 1.3–1.8 s). In the paper's own accounting (wall including
init), decimal at 142 s is **2.05 × faster** than the reproduced paper
design (291 s, §40) at 65 % of its peak host memory; binary at 189 s is
1.54 × faster at the same memory.

## 63. The two pipelines, final single-node comparison (2026-09-18)

Same code, same node (s24-16); the columns are the final three-run series
of §66 (`results/variance_b{10,2}_final/`, the grid split, the low grid
and the pool fix); the base is the only switch. "Before" = the Phase 4 acceptance (§40, host-resident, binary).

| 4 × 10¹⁰ digits, one MI300A node | Phase 4 (paper's design) | **binary, final** | **decimal, final** |
|---|---:|---:|---:|
| bs | 74.7 | 43.2 | 61.1 |
| 10dP | 9.3 | 12.2 (8.8 earlier in the allocation) | 1.7 |
| dm | 51.1 | 26.6 | 37.0 |
| dc | 82.5 | 79.3 | 4.2 |
| T1 + T2 | 7.3 | 5.7 | 4.9 |
| **phases** | **229** | **167.1 ± 2.2** (165.4 with 10dP at 8.8) | **108.9 ± 1.1** |
| wall incl. init (paper-style total) | 291 | 191.5 ± 2.2 | 133.7 ± 1.8 |
| peak host RSS | 248 GB | 233 GB (dc's host tiers) | 160 GB |
| device pools (not in RSS) | 128 GB | 128 + 120 (bs regions, reused by dm) | 128 + 117 |
| digits verified | T1, T2, 10⁹ identical | same, both dm paths | same, both dm paths |

**What each column is.** Phase 4: the paper's algorithm as reproduced
(host-resident numbers, prime-per-device tiers). Final: the WP1–WP8
changes — k-anchored Newton, device regions with subtree ownership and
the locality-aware batch tier, 3·2ᵏ lengths, seed span 256, the top bs
levels and the dm phase on device-resident numbers through the four-APU
distributed transform, `e_terms` by bisection — with the base as a switch.

**Where the bases differ, final code.** Decimal removes 10dP and dc
(90 → 6 s) and pays in bs (+17 s: seeds 10 vs 5, and the 4.44 × 10⁹-limb
top products in 6 planes where binary's 4.15 × 10⁹ take 4) and dm
(+11 s: one more Newton doubling and the same 6-vs-4 planes in A μ and
X Q). Net: decimal is **35 % faster** on one node (phases; 30 % on wall)
at **31 % less peak host memory** — 160 GB, because the last host-resident
phase of any size, dc, does not exist in decimal. Against the reproduced
paper design: 2.2 × on wall, 2.1 × on phases. The 6-vs-4 planes are the
2³¹-point cap of the device tier, not the base; a 3·2³⁰-point plane pool
(+60 GB of device memory) was the sized next step, but the grid split of
§66 reaches the same plane-point count without it. What remains of
decimal's extra cost is the seeds (+5 s, CPU) and the 6-vs-4 planes
(≈ 6 s in bs, 8 in dm).

**Multi-node (2048 nodes, 8 192 APU ranks, 2 × 400 Gb/s per APU),
sized from the measured cell.** Per 4 × 10¹⁰-per-node equivalent:
decimal ≈ 12.5 s of exposed all-to-all time (3 per product, the WP5
transform verified on real APUs and, pending WP6, across nodes over TCP);
binary ≈ 25 s (10dP and a distributed radix conversion, PLAN §15 WP2). The
distributed transform's parts on one node at 2³¹ points — local passes
0.33 s, pack/unpack 0.12, all-to-all 0.18 over three 91 GB/s links — are
the per-rank costs that scale; the fabric replaces the third. Memory per
node is the single-node profile; decimal's digits need no gather phase
(each rank formats its own limbs). Checkpoint/restart (§61) is per rank at
level boundaries.

**Recommendation for the user's decision:** the decimal base; binary
kept as the switch for reproduction of the paper. Every number above is in the run logs
(`results/variance_b{2,10}_dev2/`, `results/attr/`).

## 64. The top bs levels on the device tier — measured, not yet adopted (2026-09-18, job 20644)

`BS_DEV_MDEV=1`: the mdev-tier levels (the top two or three) hold their
nodes as device numbers and multiply through the distributed tier; the
region pools are donated to the block allocator as they free up. Decimal
4 × 10¹⁰: level 23 **4.8 s** (host mdev tier 19.5), level 24 **7.7 s**
(18.0), bs peak host RSS **160 GB** (232 — the host mdev pools are gone).
Then the run was killed in the division: the bs results and the split
temporaries fragment the donated blocks (the allocator splits blocks but
does not coalesce buddies), the reciprocal had to `hipMalloc` 129 GB on top
(22 allocations, 14 s), and the division's temporaries no longer fit.
**Then fixed the same night:** the block pool is now address-ordered
free extents with coalescing (best fit, carve from the front; `t_dbig`'s
stress test: a bs-like allocate/free pattern never grows the pool beyond
the donation and the extents merge back to one). With `BS_DEV_MDEV=1`
(job 20644's last minutes, then 20655): **decimal 4 × 10¹⁰ VERIFY OK,
phases 117.9 s** (bs 58.6, dm 48.9 — recip 17.6, division 30.3 — 10dP
1.8, dc 4.1), **peak host RSS 160 GB**; **binary 165.9 s** (dm 28.3), peak
233 GB (set by dc's host tiers now). Adopted as the default (`BS_DEV_MDEV=0`
restores the host mdev tier); the five-run variance is §62b.

## 65. WP6 — the communicator and the distributed transform across nodes (2026-09-18, jobs 20649/20656; details in `results/WP6.md`)

TCP full mesh (`comm_tcp.c`), one process per rank, launched by
`wp6run.sh` over a Slurm allocation; aac6 has one 1 GbE NIC per node, so
this is correctness only. Two nodes (s24-16 + s24-26), 8 ranks, one per
APU: `t_comm` (all-to-all of 1 B … 3 MiB slabs checked word for word,
barrier, reductions) **VERIFY OK on all 8 ranks**; `t_dist 20` (the
four-step transform, every prime, each rank checking its block-cyclic
rows against the one-rank engine) **VERIFY OK on all 8 ranks**. The job
had 22 s of overlap with the single-node allocation, enough for those
two; `t_dist 24` with 8 ranks was then run on one node with two
processes per APU (the transform does not know where a rank lives, only
the socket path differs): VERIFY OK, and size 4 over TCP up to 2²⁶
points. No source change was needed in `comm_tcp.c`, `ntt_dist.c` or
`t_dist.c`; `t_comm` gained the per-rank mode. Connection ordering
(listen first, connect upward, accept downward with a hello), port reuse
between back-to-back runs, and the no-deadlock argument for the
all-to-all (one sender thread per peer, the caller reads in rank order)
all held with 8 ranks and slabs larger than the socket buffers. A second
2-node window did not come (two of the partition's four nodes were down
all session, the third taken by another user's 8 h job); the 2²⁴ case on
two real nodes is staged on aac6 (`~/ntt-wp6`, `~/wp6watch.sh`) for the
next free window — it changes nothing in the design.

## 66. The device product split as a grid (2026-09-18, job 20655)

`rns_mul_dist_db` split a product too long for one 2³¹-point plane by
halving the longer operand recursively. For decimal's top bs product
(2.22 × 10⁹ × 2.22 × 10⁹ limbs) the halves still do not fit together
(1.11 + 1.11 > 2.15 × 10⁹ limbs), so it went to **8 planes** of 2³¹ at
78 % fill through three nested temporaries; binary's 2.07 × 10⁹-limb
operands fit at the first halving (4 planes, 97 %). The split is now a
grid: piece counts (kₐ, k_b) with ⌈nₐ/kₐ⌉ + ⌈n_b/k_b⌉ ≤ 2³¹ chosen for
the fewest plane points in total — decimal's top product becomes 2 × 3
pieces, **6 planes** at 86 %, the same total work the 3·2³⁰ plane pool
would give (4 × 3.2 × 10⁹ points) without the extra 60 GB of device
memory; one temporary instead of nested ones; binary unchanged.
Verified in `t_dbig 0 big` (the grid shapes at small sizes under a
test-only plane cap, `DIST_LOGN_TEST=24`, against the GMP-checked host
tier; the 2.2e9² case itself does not fit the node beside its host
reference) and at 4 × 10¹⁰ in both bases — digits identical to
`results/e_4e10.out`. Three runs each (`results/variance_b{10,2}_grid/`):

| 4 × 10¹⁰ | decimal, §62b | **decimal, grid** | binary, §62b | **binary, grid** |
|---|---:|---:|---:|---:|
| bs (mdev tier) | 59.6 (22.0) | 59.7 ± 0.6 (19.1) | 42.4 | 42.7 ± 1.1 |
| dm (recip / division) | 48.9 (17.6 / 30.3) | **37.9 ± 0.2** (14.8 / 22.4); 36.3 with the low grid | 28.3 | 26.5 ± 0.1 |
| **phases** | 119.2 | **108.3** (107.3–109.2) | 166.6 | **165.4** (164.1–167.1) |
| wall | 142.0 ± 1.8 | **132.0 ± 3.1** (init 18.9–22.9) | 189.1 ± 1.6 | 187.9 ± 1.9 |
| peak host RSS | 160 GB | 160 GB | 233 GB | 233 GB |

Where the 11 s went in decimal: the division's two 2.2e9²-limb products
(A μ and X Q) 8 → 6 planes each (10.8 + 10.0 s from 16.2 + 13.1), the
reciprocal's last doubling (3 × 1 pieces instead of 4), and bs level 24
(the 1.06 × 1.13e9 product was already 2 planes; level 23's pairs gain
from the single temporary). Binary's products were already 2 × 2; its dm
gain is the single temporary and the 2 × 1 split of A_top μ. The 3·2³⁰
plane pool of §63 is no longer the next item: the grid already reaches
its plane-point count.

**The low product as the same grid, pieces above the window skipped**
(`NEWTON_LOWPROD`, now default 1): the division's X Q is only needed
modulo B^w (w = n_Q + 2), so the pieces whose limbs start at or above w
are not formed. Decimal: the (1,2) piece of the 2 × 3 grid starts at
2.59 × 10⁹ > w, **5 planes instead of 6 — low product 10.0 → 8.5 s, dm
36.3 s, phases 108.3** (one run; bs 61.4 in that run, its usual 1–2 s
spread); binary: the (1,1) piece starts two limbs below w, nothing to
skip, dm 26.9 (unchanged). Digits identical in both bases; `t_dbig` checks
the low grid at three windows per shape (561 checks). The earlier
halving recursion (`rns_mul_low_db`) is gone — it needed the same 6
planes.

**A pool bug found by the test set, fixed (dbig.c):** `t_newton` with
`NEWTON_DEVICE=1` failed at 2¹⁴ limbs with a HIP "invalid argument" in
`db_to_bi`; bisected to the coalescing pool of §64 (3ee763a passes at
its parent, fails at itself). Two separate `hipMalloc` regions that
happen to be adjacent in the address space were merged into one free
extent, and a block carved across that seam is not a valid copy source.
The 4 × 10¹⁰ runs were unaffected (the pool there is the donated bs
regions, large and far apart; the small `hipMalloc` fallbacks of the
tests are the ones that land adjacent). Extents now carry their region
and never merge across regions. After the fix: `t_newton` 696 checks
(binary) and 658 (decimal) VERIFY OK, `t_dbig 0 big` 561, 10⁹ in both
bases identical to the reference.

**Final series on the closing code** (grid, low grid, pool fix; three
runs each, `results/variance_b{10,2}_final/`): decimal phases **108.9 ±
1.1** (107.8–110.0; bs 61.1, dm 37.0 ± 0.2), wall 133.7 ± 1.8, 159.9 GB;
binary phases 167.1 ± 2.2 (bs 43.2, 10dP 12.2 ± 0.4, dm 26.6, dc 79.3),
wall 191.5 ± 2.2, 232.8 GB; six of six VERIFY OK. Binary's 10dP drifted
8.8 → 12.2 s over the four-hour allocation (the host-side product into a
fresh 35 GB allocation, the page-state effect below); every device phase
is within its earlier spread.

**An observation for the run logs, not the design:** a binary run started
right after a decimal run shows 10dP ≈ 17 s and "other" ≈ 9 s (twice out
of twice today), while back-to-back binary runs give 8.8 ± 0.2 and 3.1
— the host memory the decimal run leaves behind (its 160 GB freed a
minute earlier) costs the binary run's fresh 35 GB host allocation in
page faults; it does not appear in the variance series, which run one
base at a time.

## 67. Decision: the decimal final version is the code (2026-09-18)

The user's decision on the data of §63/§66: the decimal pipeline —
limbs of 10¹⁸, the top bs levels and the Newton division on device
numbers through the four-APU distributed transform, 3·2ᵏ lengths, the
grid split — is the default configuration and `main` (fast-forward from
`wp1-decimal-base`). Reference figures, 4 × 10¹⁰ digits on one MI300A
node, three runs (`results/variance_b10_final/`): **phases 108.9 ± 1.1 s
(bs 61.1, 10dP 1.7, dm 37.0, T1 3.7, dc 4.2, T2 1.2), wall 133.7 ±
1.8 s, peak host RSS 159.9 GB**, device pools 128 GiB staging + 117 GB
regions; digits identical to `results/e_4e10.out`. `LIMB_BASE=2` runs
the paper's binary-limb pipeline on the same code (167.1 / 191.5 /
233 GB); the Phase 4 reproduction as accepted is tag `phase4-accepted`.


## 68. Phase 8 — overlap of disjoint work (2026-09-18, branch `phase8-overlap`, job 20683, s24-16; PLAN §18)

`ECALC_OVERLAP=1`: init per APU in parallel (O1); T1's P, Q term
recurrence in a background thread from the end of the seeds (O2); P and
Q left on the device by the top level and copied out, their residues
taken and A = 10ᵈ(P+Q) formed on the CPU while the reciprocal runs, Q
never copied back (O3); X copied out before the low product and the
formatting, the T2 windows and the digit residue done on the CPU during
it (O4); if a correction changes X afterwards the digits are redone.
Background threads run a bounded OpenMP team; host↔device copies and the
block pool are mutex-protected since two threads now use them.

**First run, 4 × 10¹⁰, VERIFY OK, digits identical:** wall **133.7 →
120.7 s**. Per period: init 20.2 → 15.5; seeds 10.4 → 8.1; batch levels
29.7 → 31.1 (the recurrence competes); top levels 19.1 → 17.5 (no
synchronous copy-out); reciprocal 14.8 → **19.9** (P's device blocks
stayed alive during it: pool 39 → 90 GB, 4 s of `hipMalloc` fallback in
`db_reserve`; CPU contention from the 96-thread residue/A work);
division 22.2 → 23.9 (the formatting competes with the low product's
host side); T1 3.7 → 2.6 (X's residues recomputed although the
formatting thread had them); dc + T2 5.4 → 0.1. Peak host 166.7 GB
(A's shrink in the division had been switched off by mistake; VmRSS
after bs 118 GB — the host copies of P, Q now arrive after the pools are
gone, so the bs peak is lower than before).

**What the first run hid, found over rounds 2–4 (job 20685).** Three
couplings between the background thread and the reciprocal, each fixed:
(1) the copy stream was a blocking stream, so every `hipMemcpyAsync` of
the copy-out was ordered against the reciprocal's kernels on the null
stream and vice versa (non-blocking stream, null-stream sync before a
copy of a just-computed number); (2) the dbig kernels synchronised the
whole device after each launch, so each waited for the 1 GiB copy chunk
in flight (they synchronise the null stream now; the division fell back
from 24–32 s to 21.3 s); (3) the copy-out wrote into freshly allocated
host buffers — 35 GB of page faults, 10–26 s when the page cache was full
of the 40 GB reference file from the previous comparison (the buffers are
pre-faulted in the background during the GPU levels: 0.22 s). Measuring
after a 40 GB comparison pollutes the run: the scripts now evict the file
(`posix_fadvise DONTNEED`) before a timed run — the baseline itself is
128.8 s under that condition against 133.7 in the §66 series.

**Clean pair (page cache evicted, all fixes; one run each, both VERIFY
OK, digits identical):**

| 4 × 10¹⁰ | baseline | overlapped | difference |
|---|---:|---:|---:|
| init | 19.6 | 15.3 | −4.3 |
| bs (seeds / batch / top levels) | 60.5 (11.5 / 29.9 / 18.9) | 58.2 (7.8 / 31.0 / 19.4) | −2.3 |
| reciprocal | 14.4 | 20.0 (P, Q out 5.4 s inside it) | +5.6 |
| 10dP | 1.3 | 2.4 (hidden) | — |
| division | 20.7 | 21.3 | +0.6 |
| T1 / dc / T2 | 3.4 / 4.1 / 1.3 | 1.7 / 0.1 / 0.0 | −7.0 |
| other | 2.7 | ≈ 0 | −2.7 |
| **wall** | **128.8** | **117.9** | **−10.9** |
| peak host | 160.0 GB | 154.3 GB | (pre-faulted P/Q beside bs) |

The one remaining loss is the reciprocal: the device-to-host DMA of P and
Q (35 GB) contends with its kernels for the HBM; the CPU work beside it
(residues 5.3 s, A 2.4 s) does not show. Next: the copies before the
reciprocal (1.8 s synchronous; `ECALC_OVERLAP_COPY=1` keeps them inside),
expected ≈ 114 s; then I3 (A on the device) removes P's copy altogether.

**With the copies before the reciprocal (default now; one run, VERIFY
OK):** init 15.2, bs 56.3, P/Q out 2.6 (synchronous), reciprocal 14.2,
division 21.3, T1 1.3, dc + T2 0.1 — **wall 112.1 s**, peak host
154.3 GB. Against the clean baseline of 128.8 s: **−16.7 s (−13 %)**, and
−21.6 s against the §66 series (133.7, measured behind a full page
cache). The phases that remain on the wall clock are init 15, seeds 8,
the GPU levels 48, the copies 3, the reciprocal 14, the division 21, and
≈ 1.5 s of residues at the end: the CPU-only periods are down from 40 s
to ≈ 12 (init and seeds), the rest is device time. The next overlaps are
coordinated ones (PLAN §16: I2 seeds with level 1, I3 A on the device).

**M1/M2 on the same allocation:** 2 and 4 node-processes on one node at
10⁸ and 2 at 10⁹ — meshes connected, self-test over the meshes ok, each
process its term range, node 0 combining — **VERIFY OK and digits
identical to the reference at every size**; the single-process run
unchanged.

## 69. Phase 8 I2 — the seeds computed during init (2026-09-18, jobs 20686/20687, s24-16)

The seeds need only N and the pinned staging, which init creates in its
first 4 s; the remaining ≈ 11 s of init are device-pool allocations
(`hipMalloc` at 0.057 s/GB over ≈ 240 GB of regions and planes — the
driver's page mapping, a floor no CPU work moves). So the seeds run in a
background thread started by a hook inside `rns_init` after the staging,
and `binsplit_e` takes the level-0 table from it (`binsplit_seeds_begin`;
the restart path and the host-pool path fall back to the inline seeds).
The host pool for A is no longer pre-touched in the overlapped flow (A is
formed in the background during the reciprocal, its faults hidden there).

| 4 × 10¹⁰ (clean conditions) | §68 | I2 | I2, no pool touch |
|---|---:|---:|---:|
| init | 15.2 | 21.1 | 19.9 |
| bs (seeds inside) | 56.3 (7.9) | 47.5 (0.3) | 47.5 (0.3) |
| reciprocal (incl. copies) | 16.8 | 17.1 | 16.6 |
| division | 21.3 | 21.5 | 21.6 |
| **wall** | **112.1** | 109.7 | **107.7** |
| peak host | 154.3 GB | 154.3 | **140.1 GB** |

Seed team size makes no difference (192 threads 107.7, 144 107.6; 96
109.6 with the seeds at 15 s, longer than init). Init grows 15 → 20 s
because the allocations slow beside the seed team (memory and page-table
contention), so the net is −4.4 s of the 7.9 the seeds cost; the rest of
init is the allocation floor. All runs VERIFY OK, digits identical.
I7 (seeds on the GPU) is dropped from the list: the seeds are off the
critical path.

## 70. Phase 8 I3 — the decimal division entirely on the device (2026-09-18, jobs 20688–20692, s24-16)

In base 10¹⁸, A = 10ᵈ(P+Q) = S·B^(d/18) with S = P + Q, so the division
needs no A: its "top of A" is a view of S shifted by (n_Q − 1) − d/18
limbs, and the remainder window A mod B^w (w = n_Q + 2) is zeros plus S's
lowest w − d/18 limbs (six at 4 × 10¹⁰). With P and Q left on the device
by the top level, S = P + Q is one device add, the reciprocal takes Q from
the device (seed from its top four limbs, μ tagged by the device Q), the
window and the ±Q corrections run on device numbers, and the T1 residues
of P, Q and R come from a device kernel (`db_mod_q`: one block per 16 384
limbs, thread-strided reads, per-thread Horner with B²⁵⁶, block reduction
with a table of Bᵗ, all eight primes per launch, quarters in parallel).
Nothing of P, Q, S, A or R ever exists on the host; only X and the digit
string do. The digit count is computed to the next multiple of 18 (the
requested digits are a prefix — ⌊⌊10^{d'}e⌋/10^{d'−d}⌋ = ⌊10^{d}e⌋ — the
full string is residue-checked, the requested one written and windowed),
which also removes the 10^(d mod 18) multiply. `newton_db_divmod_shifted`,
`db_set_shifted_low`, `db_mod_qs`; checks in `t_dbig` (residues against
the host Horner across quarter and chunk boundaries) and the reference
digits at 10⁸, 10⁹, 10¹⁰ with the device top levels forced
(`BS_MDEV_LOGL`), and 4 × 10¹⁰.

| 4 × 10¹⁰ (clean conditions) | I2 (§69) | I3 first | + fast residues | + one launch, fill kernel, plane tails | + temporaries freed | + exact quarters |
|---|---:|---:|---:|---:|---:|---:|
| residues P, Q (device) | (host, hidden) | 3.5 | 2.1 | 1.3 | 1.2 | 1.0 |
| reciprocal (proper) | 14.2 | 18.6 | 16.8 | 17.3 | 16.9 | 14.9 |
| division | 21.6 | 25.2 | 24.7 | 23.5 | 22.1 | 20.8 |
| top bs levels | 17.2 | 17.4 | 17.2 | 17.5 | 17.6 | 14.6 |
| pool fallback (hipMalloc inside the phases) | 90 GB | 116 | 116 | 116 | 90 | 27 |
| **wall** | 107.7 | 116.3 | 114.1 | 113.0 | 108.1 | **104.8** |
| peak host | 140.1 GB | **74.5 GB** | 74.5 | 74.5 | 74.5 | 74.5 |

The host peak halves (the staging, X and the digit string remain). The
time went up first because the device division keeps more large numbers
alive at once and each quarter is a 2ˡ/3·2ˡ class: S, Q, μ, the 51 GB
product, X, X·Q, the window and R overflowed the donated regions and the
block pool fell back to `hipMalloc` for ≈ 116 GB inside the phase
(0.057 s/GB — this, not compute, was the "window 2.8 s" and the slower
reciprocal). Fixes in order: residues coalesced (2.0 → 0.07 s at 10⁹),
all primes in one launch, the window filled by a kernel (hipMemset runs
at ~10 GB/s here), the plane pools' unused tails (17 GB) donated to the
block pool as borrowed regions, and every temporary freed at its last use.
Two more findings: `hipMalloc` cannot be hidden behind GPU work — 96 GB
mapped from a background thread during the batch levels slowed them
29.7 → 35.9 s (the page mapping stalls the running kernels), so the
pregrow stays off (`ECALC_POOL_GROW_GB`); and the quarter size classes
(2ˡ / 3·2ˡ limbs) rounded 2.22e9-limb numbers up by 45 %, which was most
of the overflow — quarters are now exact (multiples of 4096 limbs, the
quarter found by three comparisons instead of a shift and a division by
3), the pool coalescing any sizes. Final: **104.8 s wall, 74.5 GB peak
host**, all runs VERIFY OK and digits identical; `t_dbig 0 big` 609
checks, `t_newton` 658.

## 71. Phase 8 — the new baseline pinned, and the single-node ceiling (2026-09-18, job 20696, s24-16)

Three runs of `main` @ 62d81bb (overlap, seeds in init, division on
device, exact quarters), clean conditions, `results/variance_b10_p8/`:

| 4 × 10¹⁰ | run 1 | run 2 | run 3 | mean ± sd |
|---|---:|---:|---:|---:|
| init | 17.7 | 18.8 | 18.4 | 18.3 ± 0.6 |
| bs (batch / top levels) | 45.2 (30.2 / 14.7) | 45.1 (30.4 / 14.4) | 44.7 (29.6 / 14.7) | 45.0 ± 0.3 |
| dm (reciprocal / division) | 37.5 (15.8) | 37.4 (16.0) | 37.8 (16.1) | 37.6 ± 0.2 |
| phases | 82.8 | 82.7 | 82.6 | 82.7 ± 0.1 |
| **wall** | 100.5 | 101.5 | 101.1 | **101.0 ± 0.5** |
| peak host | 74.5 GB | 74.5 | 74.5 | **74.5 GB** |

All VERIFY OK, digits identical to the reference. The binary path
(`LIMB_BASE=2`, 10⁹) is still byte-identical after all of Phase 8.
Against the day's starting point (§66/§67: 108.9 s phases, 133.7 s wall,
160 GB) the wall is −24 %, the host memory −53 %; against the reproduced
paper design (291 s, 248 GB): 2.9 × and 30 %.

**The ceiling.** 5 × 10¹⁰: 125.8 s, 85.2 GB, identical to the 5 × 10¹⁰
reference of §45 (which the paper design produced in 442 s at 375 GB).
6 × 10¹⁰: 159.7 s, 99.7 GB, VERIFY OK (T1 residues, T2 windows; the
first run at this size). **7 × 10¹⁰: 182.7 s, 114.1 GB, VERIFY OK** —
init 24.5, bs 76.9 (batch 47.2, top levels 29.2), dm 81.1 (reciprocal
31.0: the dm phase's block pool overflows the donated regions again at
this size and grows by `hipMalloc`; division 50.1). Device: 192 GB of
regions + 137 GB of planes + the block-pool growth. 8 × 10¹⁰ would need
≈ 580 GB (device ≈ 137 GB + 4.7 GB per 10⁹ digits, host ≈ 69 GB pinned +
1.3 GB per 10⁹) against the node's 502 GB, so **7 × 10¹⁰ is the
single-node ceiling** of this pipeline — 1.4 × the paper design's 5 × 10¹⁰
at a third of its host memory; at 2 048 nodes that is 1.4 × 10¹⁴ digits per
run before any memory work. Time per digit grows mildly with size (2.5 →
2.6 ns/digit from 4 to 7 × 10¹⁰: the reciprocal's pool growth and the
2³¹-point plane cap in the top levels).

## 72. Phase 8 step 3 — init: the pinned staging sized to its use (2026-09-18, job 20703, s24-26)

In the decimal device flow the 64 GiB of pinned staging (16 GiB per APU,
the paper's) serves only the seeds (each region's spans, ≈ 10 GB at
4 × 10¹⁰) and the checkpoint copies; it is now sized to the largest
region's seed stage rounded up to 1 GiB (`binsplit_seed_stage_bytes`,
`ECALC_STAGING=0` restores the paper's size; the binary path and the
multi-node path keep it). 4 × 10¹⁰: init 18.3 → **15.3 s**, wall
**98.5 s**, peak host **70.8 GB** (RSS during bs 47 GB); 10⁹: staging
2 GiB, peak host 73 → 16 GB. Digits identical.

Tried and rejected: region slack 1/16 instead of 1/4 (`BS_REGION_SLACK`).
The tree's node counts are not powers of two, so at the level with five
output nodes region 0 holds two of them — 40 % of the level — and its
pool must grow by `hipMalloc` inside the phase; with the smaller slack
both parities grew (101.3 s). At the default slack the same imbalance
costs one 19 GB growth (≈ 1.3 s) in every 4 × 10¹⁰ run; sizing the
regions for it (0.4 of the level instead of 0.31) costs about as much at
init, so it is recorded as an item (PLAN §16 I15), not changed.

## 73. Phase 8 M3 — the top levels of the tree as distributed products over node groups (2026-09-18/19; `results/M3.md`)

Built by an agent on branch `m3-dist-top-levels`, merged. The structure:
a **layered communicator** (`comm_layered.c`) composes the four APUs
over xGMI with the inter-node mesh d into one communicator of 4g ranks
(rank ρ = g·d + node, APU-major, so a sender's slabs for the APUs of all
nodes are contiguous and the intra-node stage is the existing push kernel
with slabs of g × bytes) — the hierarchical all-to-all of M7 from the
start; **sharded numbers** (`mdb`: n, a sharding basis N, the group,
each node's contiguous share in its own dbig); **the product over a
group** (`rns_mul_dist_mn`): each share meets a rank's column-major row
sequence in exactly one segment, so an operand is redistributed by one
all-to-all-sized exchange with a g-entry segment table, the four-step
transform runs over the layered communicator (three layered all-to-alls
per prime), the striped CRT writes the rank's result sequence, one
reverse exchange places the shares, the stripe spills are all-gathered
and added by a fixed-length dbig add whose (carry, propagate) flags are
scanned across nodes; the tree add P = P_A Q_B + P_B rides as the CRT's
x operand. **The tree** (`mn_tree`): level ℓ pairs node groups of
2^(ℓ−1) (lower terms × higher terms), results sharded over the joined
group; per-level group meshes on unique port slots; at the end node 0
gathers the shares and runs the single-node division and output (until
M4/M5). `MN_COMBINE=host` keeps M2's centralised combine.

**Verified** (all digits byte-identical to the references, VERIFY OK):
the layered communicator at 2, 3, 4 node-processes up to 2²⁶ points;
ecalc at 10⁶ (sizes 2, 3, 4), 10⁸ (sizes 1, 2, 4 on one node; with the
leaf's device top levels; **2 and 3 node-processes on 2 and 3 real
nodes**), 10⁹ (sizes 1, 2, 4; with the device top levels across real
nodes). Size 1 is the unchanged single-process path. Timing on the 1 GbE
fabric is meaningless (10⁸ over two real nodes 11.5 s of which the tree
6.3).

Open (M3.md): no grid split over shares for products beyond
2^(31+log₂ g_t) points (needed at 4 × 10¹⁰ below size 8); descriptor and
flag all-gathers are O(g) point-to-point loops (an `allgather` op, M8);
per-level TCP meshes (RDMA sub-communicators later); no load balance at
non-power-of-two sizes; the packed slabs' memory (≈ 4 q per APU from the
block pool) to be accounted in M9. The division and the output remain
node 0's (M4, M5).

## 74. Phase 9 — seven agents in parallel: what landed (2026-09-19; results/A-*.md, N-kernel.md)

PLAN §19 executed with seven agents in their own worktrees and aac6
clones, the main session integrating (merge order by readiness, `main`
re-verified after the merges). Each agent's design, tests and open
issues are in its `results/<agent>.md`; the integration facts:

| agent | delivered | gate | merged |
|---|---|---|---|
| A-comm (M7 + allgather) | `allgather` in every transport (layered: inter first, 4× less on the fabric); slab pipelining with two exchanges in flight; 15–37 % of the xGMI exchange hidden | t_comm/t_dist all modes at 2–4 node-processes; 10⁸ sizes 2, 4 identical | cf90a2a |
| A-ckpt (M6) | per-node checkpoints: leaf sets per rank, tree-level sets of the shares, device-number levels included; v1 format still read | restart identical from every kind of set at sizes 1, 2, 4 (10⁸, 10⁹), kills mid-write | 07af461 |
| A-div (M4 + B3 + C3) | the reciprocal and division over the whole machine on sharded numbers (`mdb_shift/addsub/cmp/mod_qs`, `recip_mn`, `newton_mn_divmod`); single node: the high product's pieces below the cut skipped | sizes 1–4 on one node and 2/4 on two real nodes identical; **single node 91.2 s** (dm 37.6 → 32.8) | ba7f027 |
| A-out (M5 + C1) | each node formats and writes its part of X streamed in 256 MB chunks; rank-local T1 (term recurrence over the node's range, share residues by kernel) reduced over the nodes; T2 by the owning node; `t_out` 2 860 checks | sizes 1–4 and two real nodes identical; **single node 95.2 s, host 48.7 GB** (no 40 GB string) | aa3bb56 |
| A-grid (A3 + C5) | the grid over shares (piece views, shifted distributed add with the cross-node carry scan), the low product over shares; an M3 carry-scan bug (empty shares) fixed; C5 measured: 3·2³⁰ planes −2.5 s for +34 GiB/APU, declined | `t_mn_grid` 120 checks at 2/3/4 processes; 2 × 10¹⁰ at size 2 with the grid forced identical; 4 × 10¹⁰ at size 2 needs two real nodes (memory) | 97281fd |
| N-kernel (B1 + B4) | paired operand in the batch tier for 2ᵏ and 3·2ᵏ lengths (B once per pair, M/2 transforms); the radix-3 inverse fuses the pointwise; the register-blocked body found not to be ecalc's default — now it is (bit-identical); the DPP exchange measured slower, declined | t_ntt 593, t_mul 189/72/102, t_bs; 10⁹ identical both bases; batch tier 30.5 → 25.9 s | 2897396 |
| A-mem (M9 + C4 + C2) | `mem_report` per phase and node (device by category, host by item); plane pool 1 at 3q, region pools from one arena per device sized from a simulated layout (216 GB mapped at init vs 249); levels with ≤ 16 nodes laid out round robin (no region growth in bs); seed-sized staging at every size; a batch-tier plane-growth bug caught; capacity per node at sizes 1/2/4 | 8 runs at 4 × 10¹⁰ identical, init 17.7 → 15.5; 10¹⁰ at sizes 2 and 4 now fit one node | 38ed61b |

Two merge conflicts (additive, both sides kept), one scoping fix, and one
integration defect found by the re-verification: in the distributed
division's flow the non-zero nodes exited after the gather while the
per-node output stage expected them (node 0's scatter of X met a closed
mesh) — one line, every node goes to the output stage.

**The merged single node (`main` @ 38ed61b, all seven agents; clean
conditions, jobs 20757–20758, `results/variance_b10_p9/`): 4 × 10¹⁰ in
88.3 / 86.7 / 85.9 / 84.8 / 86.2 s — wall 86.4 ± 1.3 s, phases 70.3 ± 0.5
(init 16.1 ± 0.9; bs 37.3 = batch 23.5 + top levels 13.5; dm 32.9 =
reciprocal 16.2 + division 16.7; T1/dc/T2 hidden), peak host 48.8 GB;
five of five VERIFY OK, digits identical.** Against the day's start
(98.5 s / 70.8 GB): −12 %, −31 %; against §71's 101.0 s: −14 %; against
the reproduced paper design (291 s / 248 GB): **3.4 × at 20 % of the
memory**.

**The multi-node pipeline end to end on the merged tree** (one node,
node-processes sharing it; every node prints its own VERIFY and node 0
the all-reduced one; part files concatenated and compared): 10⁸ at
sizes 2, 3, 4 and with the device top levels; 10⁹ at sizes 2 and 4;
checkpoint + restart at size 2 from a leaf set; **10¹⁰ over four
node-processes (125 s over loopback TCP, 40.5 GB per process)** — all
identical / VERIFY OK on every node. Two real nodes were verified by the
agents (A-div, A-out: 10⁸ and 10⁹ at sizes 2 and 4). 4 × 10¹⁰ over two
real nodes remains to be run when two are idle together (memory). Unit tests on the merged tree: t_ntt 565,
t_mul 189, t_bs 10, t_dbig 555, t_newton 620, t_verify 334 — all OK;
the binary path 10⁹ still byte-identical.

**The ceiling on the final code:** 7 × 10¹⁰ digits in **163.8 s** (was
182.7 in §71), peak host **76.5 GB** (was 114.1), VERIFY OK — init 25.3
(170 GB of regions mapped), bs 65.8 (batch 39.5, top levels 25.7), dm
72.6 (reciprocal 35.3: the block pool still overflows at this size, PLAN
§16 I16). With the host at 77 GB the next limit at 8 × 10¹⁰ is device
memory (≈ 195 GB of regions + 120 GB of planes + the dm pool against
502 GB shared with the host), as before.

## 75. Phase 10 — five agents on the §20 backlog: what landed (2026-09-20; results/G.md, H.md, M.md, C.md, T.md)

PLAN §21 executed as §19 was: five agents in their own worktrees and
aac6 clones, the integrator merging in readiness order (G → H → T → C →
M) and re-verifying `main` with T's regression script after the merges.
Each agent's design, measurements and open issues are in its
`results/<agent>.md`; the integration facts:

| agent | delivered | gate | merged |
|---|---|---|---|
| G grid/dm (A5, A1, A6) | the division's high/low cuts over shares (adopted); the transform cache over shares (adopted at size > 1 — `RNS_DIST_CACHE_MN=2`; at size 1 measured and not adopted); the pointwise product fused into the distributed inverse's column pass (`dist_inv_pw`, bit-identical, −1 s) | 10⁸/10⁹ sizes 1–4 identical; `t_mn_grid`, `t_dbig big` green; 4 × 10¹⁰ 84.8 s identical | 722dc3f |
| H host memory (B1, B2) | X never on the host: the writer and the residue kernel read the device X (`newton_db_x_dev`), corrections in place; at size > 1 each node reads its share of the sharded X; the seeds streamed by the seed thread straight into the region arenas (`BS_SEED_DIRECT=1`; two pinned 2 GiB buffers only before the arenas exist), the init staging 1 GiB/APU | 10⁹ identical; sizes 2–4 identical incl. `MN_DM=host`; **4 × 10¹⁰ 82.7 s, host peak 11.7 GB** (init 45.7 → 11.5, bs 47.4 → 9.0, dm 70.8 → 7.5 GB) | 9bf8f16 |
| T tests & docs (D3, D2, D4, D5, C6, E3) | `mnaccept.sh` — the standing regression (unit tests, 10⁹ both bases, 10⁸ at sizes 2/3/4, 10⁹ at 2/4, restart at size 2, `--full` adds 4 × 10¹⁰ with the reference evicted); tree-level checkpoints every k levels with the barrier off the critical path (`BS_CKPT_TREE_EVERY`); README for the Phase 9/10 code | 17/17 on its branch (4 × 10¹⁰ 89.5 s identical); 10¹⁰ at size 4 restarted from tree-level sets identical; `LIMB_BASE=2` at 10¹⁰ identical; D5: 18 forced-growth runs at 10¹⁰/4, bs deterministic — not a stale pool pointer; the one failure has A-mem's per-prime signature → the size > 1 residue path, open | e9ed73b |
| C communication (B7, C4, C5) | `comm_alltoallv` (+ `_host`, `comm_prefix`) in every transport with count checks (the consumers in `rns_dist.c`/`newton_db.c` are the next step, exact code in C.md); the second pipeline plane `dist_fwd2/inv2` implemented and **rejected** (3–6 % slower at 2³⁰–2³¹: the unpacks contend with the push kernel); instead 64-bit push stores and 76 blocks per peer — **xGMI 2³¹ convolution 0.174 → 0.158 s (−9 %)**, new default (`COMM_PUSH64=0 COMM_PUSH_BLOCKS=228` restores); `DIST_STATS` by events under pipelining (exposed exchange, hidden fraction) | t_comm 2/3/4/8, t_dist every mode at 2–4 node-processes, xGMI 31 runs at 2³¹; 10⁸ sizes 2/4 and 10⁹ identical | 246d1e4 |
| M device memory (B5, B6, B4/A3, 8 × 10¹⁰) | every rank releases its arenas (B5); the leaf's parities donated to the block pool before the tree at size > 1 (tree hipMalloc 11–44 → 0–5 GB per process); per-fallback pool logging, `mem_oom`, a `division` row; the finding that the dm phase's in-phase growth is exactly one block per APU — t1's quarter (8.9 / 15.6 / 17.8 GB at 4/7/8 × 10¹⁰) that finds no contiguous hole — and C3's chunk sized to hold it; the seed-staging cap lifted | 4 × 10¹⁰ six runs identical (85.3–88.8 s); **7 × 10¹⁰ 159.7 s default, 157.2 s with `ECALC_DM_POOL=1`** (kept off — the user's decision); **8 × 10¹⁰ on one node VERIFY OK in 210.4 s, host 90 GB, node peak 393 of 502 GB**; 10¹⁰ at size 4 identical | 1f0b114 |

Three merge conflicts, each both sides kept: `newton_db.c` (G's cache
release beside H's device X), `ntt_dist.c` (G's fused inverse on C's
stats and `inv_prod`), `ecalc.c` (H's streamed-seed staging without M's
cap; H's pool release after the output stage with M's `division` row).

**Regression on the merged tree** (`mnaccept.sh --full`): after G + H
(9bf8f16, job 20789) 17/17, 4 × 10¹⁰ identical in 84.7 s; after all five
(1f0b114, job 20791) 17/17, 4 × 10¹⁰ identical in 85.1 s (1 906 s for the whole script).

**The closing series** (`main` @ 1f0b114, `closing.sh`: the reference
evicted before every run, digits compared after each; job 20792, s24-26,
`results/close10/`): **4 × 10¹⁰ in 84.8 / 81.5 / 82.8 / 82.5 / 83.6 s — wall 83.0 ± 1.2 s**,
phases 66.7 ± 0.3 (init 16.4 ± 1.1; bs 36.3 = batch 23.0 + top levels
13.1; dm 30.1 = reciprocal 15.2 + division 14.9; T1/dc/T2 hidden),
**peak host 11.7 GB** (staging 8.6 pinned + 2.9 other; no X, no string),
device 262 GB mapped at the dm peak (216.6 at init: planes 120.3,
regions 95.7); five of five VERIFY OK; runs 1–4 digits identical (run
5's comparison was cut by the allocation's release after its VERIFY OK —
its wall stands, its digits are unverified against the file).

Against Phase 9 (86.4 ± 1.3 s / 48.8 GB): −4 % wall, −76 % host memory; against the
reproduced paper design (291 s / 248 GB): **3.5 × at 5 % of the host memory**.

**Not done this session:** C2/D1 (4 × 10¹⁰ / 5 × 10¹⁰ over two real
nodes — no two idle nodes, and §20 C2's 1 GbE argument stands); the
`alltoallv` consumers (G's files; C.md has the code); E1 (deleting the
rejected forms: the host-flow stand-ins, `dist_fwd2/inv2`, the DPP
exchange) and E2 remain proposals for the user. Open issues per agent
in the five `results/*.md`.

## 76. Phase 11 — six agents on the three priorities, for the 576-node target (2026-09-20/21; results/{S,L,X,P,V,M11}.md)

PLAN §26 executed as §21 was, against the target of PLAN §25 (576 MI300A
nodes = 9 · 2⁶, Slingshot-2 dragonfly, two 400 Gb/s NICs per APU, SHMEM).
Merge order by readiness (L → X → S → M → P → V), `mnaccept.sh --full`
on `main` after the merges. The integration facts:

| agent | delivered | gate | merged |
|---|---|---|---|
| L layout at 576 (C3, B7) | the distributed transform balanced over a group of **any size** (rank ρ = g·d + r holds ⌊R/4g⌋ or ⌈R/4g⌉ rows; power-of-two g keeps the pipelined path bit for bit, other g runs a general four-step with per-pair slabs on `alltoallv`); `MN_GROUPS` level→group schedule (`2,4,…,64,576` default, or `…,64,192,576`); the four consumers on `alltoallv`/`allgather` — the padded g × slab scratch is gone | `t_mn_grid` at 2, 3, 5, 6, 9 processes; 10⁸ at sizes 3, 6, 9 and 10⁹ at 3 and 6 identical; 10¹⁰ at size 4 identical; size 1 identical | 3b956a3 |
| X fabric (X1, X2) | `mn_model.py` — the per-node model of the multi-node run on the target fabric (walls, bytes per NIC and per global link, exposed communication, memory), calibrated on aac6 at g = 2, 4 (10⁹ ±2 %, 10¹⁰ −10 %); the reciprocal's early doublings on the smallest prefix group [0, 2^L) by a cost rule (`NEWTON_MN_GROUPS`) — at 576 nodes the reciprocal 16.0 → 13.3 s modelled, messages per APU 1.5 M → 0.55 M; the dragonfly third layer modelled: does not pay at 576 (cross-group bytes relayed twice) | sizes 2–4 at 10⁸/10⁹ identical with X1 on; 10¹⁰ at size 4 identical and not slower (100.6 s) | ebac93c |
| S transport (M8-s) | `comm_shmem.c` on OpenSHMEM 1.4 only (symmetric pool HIP-registered, one context per communicator, `putmem_nbi` + `quiet` + signal, `wait_until`; all-to-all/-v, all-gather, barrier, max, sum-mod-q, point-to-point); per-level PE sets (the teams shim) replace the per-level TCP meshes and port slots; `mnrun.sh` by `srun --mpi=pmix`; the dragonfly third layer of the layered all-to-all (`MN_TOPO_GROUP`) on both transports. OSHMEM 4.1.6 traps documented (ASLR crash without `setarch -L`, `fence` not ordering nbi puts, THREAD_MULTIPLE crashes → serial default) | `t_comm` at 2/4/8 PEs, `t_dist` every mode incl. layered at 8 with `MN_TOPO_GROUP=4`; 10⁸ sizes 2/3/4 and 10⁹ sizes 2/4 identical over SHMEM; **`mnaccept.sh` with `COMM_TRANSPORT=shmem`: 16/16** | 826cd58 |
| M memory (§23-5, model, 10¹¹) | the arena's tail reserved for t₁'s quarter as a pool policy (`ECALC_TAIL`, v4: large requests carved from the back, small ones kept out of it) — **zero `hipMalloc` inside bs, the reciprocal and the division at 4, 8, 10 × 10¹⁰**; the tree's slabs at size > 1 from the arena (5 GB/process → 0); `mem_model.py` (`mem_per_node(D, g)`), exact against the runs | 4 × 10¹⁰ identical, device 253 GB at the dm peak (was 262); **8 × 10¹⁰ node peak 382 GB (was 393)**; **10¹¹ digits on one node: VERIFY OK, 262.9 s, node peak 445 of 502 GB**; sizes 2/4 identical, 10¹⁰ at size 4 identical | 1efe1b8 |
| P single-node speed (B3, A2, §23-3, I11, A4) | 3·2³⁰-point planes sized at init (`RNS_PLANES_3Q30`): phases −3.6 s but the 60 GB more of pools cost +4.7…6.3 s of driver mapping at init → **default off** (measured, switch kept; fastest phases measured 62.4 s); level-22 products paired in the striped batch path, −0.55 s, adopted; `ECALC_DM_POOL` on at ≥ 5 × 10¹⁰: **7 × 10¹⁰ in 153.5 s** (was 159.7); context tables and twiddles uploaded once (bit-identical); the tile budget knob (no gain) | 10⁹ identical both bases; 10⁸ sizes 2/4 identical; **4 × 10¹⁰ five runs 82.0 / 82.2 / 82.7 / 79.1 / 81.3 — 81.5 ± 1.4 s, phases 66.0 ± 0.4**, identical (target ≤ 79 not met: init's mapping is the spread) | fba199d |
| V verification (D5, recheck, E1 c, E2) | **the T1 moduli corrected**: `verify.c`'s eight "primes above 2⁶²" were 2⁶² + {135, 179, 183, 247, 315, 319, 349, 397} — only the first is prime; two composites have all factors < 2.5 × 10⁷ so every Q ≡ 0 modulo them, which is what made the failures read "BAD at six, ok at q2/q6" (and every historical failure had wrong digits — §75's "digits right, checker wrong" was mistaken); now the true first eight primes 2⁶² + {135, 169, 177, 187, 189, 193, 253, 277} (§-table), digits unchanged. **A real fault localised, not fixed**: under T's forced-growth recipe (10¹⁰/4, pool 1 forced to grow) node 0 or 1's leaf P_r and Q_r come out wrong from one limb up — 57 runs: 21/26 right without a per-level probe, 31/31 with one → a timing-dependent error at the batch tier's level transition (`spill_merge`'s CPU→GPU hand-over before the next scatter is the first suspect); never at the defaults (no growth). Reproducer `v11_d5.sh`, `ECALC_RES_LOG`, `ECALC_LEAF_DUMP`. `ECALC_RECHECK=1` re-verifies a finished run from the digit file, the `.t1` sidecar and the checkpointed top-level P, Q (RECHECK OK / FAILED on a flipped digit). Deleted: `DIST_PLANE2`, `BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY`; the host-flow stand-ins kept as the cross-check. `results/*.md` tracked | 17/17 on the branch (4 × 10¹⁰ identical, 86.2 s); recheck identical to the in-run check at 10⁸/10⁹ sizes 1, 2 | 06e06f1 |

Conflicts: `rns_dist.c` (P's radix-3 argument on L's any-size cap;
both kept). **Regression on the merged tree:** after L+X+S (826cd58,
job 20821) 17/17, 4 × 10¹⁰ identical; after +M+P (fba199d, job 20831)
17/17, 4 × 10¹⁰ identical in 82.1 s; after all six (06e06f1, job 20832) 17/17, 4 × 10¹⁰ identical in 80.7 s.

**Findings that change the plan** (details in the agents' files):
0. *The verification was weaker than stated until now.* Seven of the
   eight T1 moduli were composite (V); the check still caught every
   wrong result that occurred (all failures had wrong digits), but the
   2⁻⁴⁹⁶ figure did not hold and the per-prime pattern misled §75. With
   the true primes the claim holds. And a timing-dependent fault in the
   batch tier's level transition exists under forced pool growth (never
   at the defaults) — the first correctness item for the next session.
1. *The fabric is not the bottleneck at 576.* X's model: 13–15 % of the
   per-node wall exposed at every size; the eight distributed tree levels
   are 34–40 % of it. The dragonfly third layer does not pay (it doubles
   the NIC bytes); the small-group doublings do.
2. *The top-level product's scratch is the memory line at 576, not the
   exchange.* M's accounting: with the code as it is the top product is
   one transform whose plane pools grow to n/4g (214–856 GB per node)
   plus spill buffers, which limits 576 nodes to **1.1–1.25 × 10¹³
   digits (1.9–2.2 × 10¹⁰ per node)**; with that product piece-gridded
   over the 2³¹-point planes — the form the single-node reciprocal
   already uses — the per-node profile is the single node's and the
   limit is **≈ 4.4–4.6 × 10¹³ (7.7–8 × 10¹⁰ per node)**. This is the
   first item for the next session.
3. *The larger planes are a wash on this node:* the mapping rate under
   the seed stream (0.08–0.12 s/GB) costs more than the transforms gain;
   the tail layout or moving the seeds out of the mapping window would
   turn it into a net ≈ −1.5 s.

**576-node estimate (standing rule; modelled from measured per-node
runs, X's `mn_model.py` and M's `mem_model.py`):** with the code as
merged, **≈ 1.2 × 10¹³ digits in ≈ 1.5–2 min**; with the top product
gridded (not yet written), **≈ 4.4 × 10¹³ digits in ≈ 4.6 min per-node
wall** (7.7 × 10¹⁰ per node at 500 of 502 GB; the safe size 2.2 × 10¹³
in 2.0 min). Labelled: per-node compute measured (8 × 10¹⁰ in 195 s,
10¹¹ in 263 s on one node); fabric and cross-node memory modelled;
the SHMEM per-message cost and the part-file bandwidth assumed.

## 77. Phase 12 — the complete solutions to the open design choices (2026-09-21/22; results/{R,G12,I,S12,Q,W}.md)

PLAN §27 executed with six agents against the target of §25 (576 nodes,
Slingshot-2 dragonfly, 8 NICs per node, SHMEM). Each row was given the
*complete* solution rather than the cheapest, and every claim below is a
measurement on aac6. Merge order Q → S → W → G → I → R, the regression
after the merges; `main` @ 3524146.

| agent | delivered | gate | merged |
|---|---|---|---|
| **R** the race | **The cause named, and it is a platform property**: on this ROCm (7.2.4, MI300A) a *same-device* device-to-device `hipMemcpy` returns **before the copy has run** (a peer copy is host-synchronous). The level loop's odd-node copy issued one such copy on the destination's null stream and nothing waited; the next level's scatter kernels on the other three APUs read the shared operand 0.1–0.2 ms later, while the copy was still landing (0.2–3 ms). Hence the signature (P wrong from limb ≈ 2¹⁷, Q wrong from that limb + 3 961 286 — the trailing-zero counts of the two operands: one stale operand, not two errors) and why any per-level probe hid it. Fix: `mem_dev_copy_on` waits for its copy. Evidence: `tests/t_copy_order` (same-device copy returns in 0.000 s and is corrupted 5/5; with the wait 0/5) and `ECALC_COPY_PROBE` (47 of 52 same-region copies completed after the next level's first kernel). Also: in-phase pool growth now aborts with the accounting unless `RNS_POOL_GROW=1`, and `mnaccept.sh --stress` runs ten forced-growth runs | **56/56** forced-growth runs at 10¹⁰/4 with no probe (24/26 unfixed); `--stress` 30/30 in three batches; regression 18/18, 4 × 10¹⁰ identical | 6b4ddba |
| **G** the top product at scale | The tree's top levels as **piece grids over fixed planes for any g**, walking the `MN_GROUPS` schedule with k-way Horner combines (576 → 2,…,64,192,576); the spill all-gather (g × 4C limbs per APU) replaced by an exact `alltoallv` (≈ 4C + 4g limbs, constant in g); `binsplit.c`'s arena request — which sized the top level as one uncapped transform (≈ 700 GB per node at 576 × 4 × 10¹⁰, so the run died at init) — re-derived from the gridded scratch. Found and fixed `mdb_add_shifted` sizing its rounds by the windows of nodes 0…g−1 instead of g₀…g₀+g−1 (on a group not starting at node 0, X was never added; only caller is the gridded product, so no earlier result is affected) | `t_mn_grid` 200–224 checks at 2/3/4/6/9 processes; 10⁸ at sizes 3, 6, 9 and 10⁹ at 2, 3, 4 gridded, identical; **10¹⁰ at size 4: 123.3 s gridded vs 124.3 s not, identical, tree hipMalloc 0**; modelled node peak at 576 × 4 × 10¹⁰ **1170 → 354 GB**, per-node ceiling 1.9 × 10¹⁰ → **7.1 × 10¹⁰** | 7ece3db |
| **I** the init floor | Every allocation form measured on the APU (`tests/t_alloc`, 50 GB × 4): **nothing beats `hipMalloc` in a fresh process** — 0.057–0.072 s/GB for the device forms, managed 0.114, host-backed 0.091, 4 KiB mmap 0.68; the cost is the kernel clearing pages on the allocating thread (~14 GB/s per core) serialised by one lock, and every host-backed form drops `hipMemcpy` to 21 GB/s (SDMA) from 1.4–1.6 TB/s, disqualifying it. Only re-allocation of memory the same process freed is cheaper (0.035 s/GB). The seed overlap is also already right (81.5 s against 88.5 / 88.3 for seeds-first / seeds-after). What did pay: on M11's tail layout the 3·2³⁰ planes gain 5–6 s of phases for +4 s of init, so `RNS_PLANES_3Q30`'s size rule is now the default. `ECALC_DM_POOL` deleted (a no-op with the tail) | 10⁹ identical both bases; six 4 × 10¹⁰ identical; two 8 × 10¹⁰ VERIFY OK, node peak ≈ 382 GB, zero in-phase `hipMalloc`; `t_ntt` rates unchanged | fe902da |
| **S** the transport's target forms | **Sandia OpenSHMEM built in user space on aac6** (SOS + libfabric 1.20.1, `srun --mpi=pmi2`, sockets provider) so the forms the target needs are tested for real, not just compiled: one context per communicator with `COMM_SHMEM_SERIAL=0` (concurrent waits), `shmem_ctx_putmem_signal_nbi` ordering, a HIP device buffer as the symmetric heap (a small SOS patch adds the external-heap hook), and `comm_sym_alloc` — the callers' slabs resident in the symmetric pool, so puts go sender-slab → receiver-slab with no staging and no helper thread. Q's finding acted on: the staging is released per exchange (it was held per communicator: ≈ 345 GB per node at 576). One real bug found by the target form: a pool-resident receive buffer published before the caller's stream had finished with it | `t_comm` at 2/4/8 PEs and `t_dist` in every mode on SOS in the target forms and on OSHMEM; 10⁸ at sizes 2, 3, 4 and 10⁹ at 2, 4 identical over SOS on both host and device heaps; **the regression over SOS 16/16**; the third layer measured +16–20 % on one node (no global links to save) and stays off | bc26f6a |
| **Q** the target plan | `mn_model.py` with L's real schedule (each k-way level costed as the tree folds it) — **decision: `MN_GROUPS=2,4,8,16,32,64,192,576` (3·3), −5 % against the 9-way and fewest global-link bytes**; `mem_model.py` for both tree forms; **`estimate.py`**: `estimate(g, D)` printing digits, minutes, GB per node, TB per NIC and on global links, and whether it fits, every column labelled measured / modelled / assumed; **`docs/TARGET.md`**, the run recipe for the target (build, every variable with its target value, the `srun` line, the sizes in order, checkpoints, the recheck, what to measure first to calibrate, eleven traps) with every named switch grep-verified to exist | the model within **6.1 %** of all 16 recorded aac6 points | 49d2623 |
| **W** verification | `ECALC_CKPT_TOP` writing the top-level P, Q in the background (P under the reciprocal, Q under the division) so a finished run can be re-verified without recomputing; the recheck as a regression step (and a corrupted copy must fail); the recheck itself made a single pass over the digit file with a reader thread (98 → 46 s at 4 × 10¹⁰); `ecalc/README.md` brought to one grep-verified switch list | regression 20/20 with the recheck steps; a 4 × 10¹⁰ run rechecked from its files in 46 s | 6dd6349 |

**An integration defect caught by the closing measurement.** W's default
(the top set written for runs above 10¹⁰) is hidden only where the disk
writes at ≳ 1 GB/s, as measured on its node; on aac6's slower path the
35.56 GB set runs at **0.31 GB/s — 113 s that the division waits for**:
the 4 × 10¹⁰ wall went 81 → 164 s (regression job 20964, `dm` 26 → 113 s)
with the digits identical. The default is therefore **off** (3524146);
the feature is one switch away, and making the division release Q before
the write finishes is the open item.

**Regression on the merged tree**: after Q+S+W+G+I (fe902da, job 20952)
**20/20**; after all six with the stress step (6b4ddba, job 20964)
**21/21** — including ten forced-growth runs, the checkpoint restart, and
the 4 × 10¹⁰ recheck. A node failure (s24-30 rebooted mid-run, NODE_FAIL)
destroyed one earlier attempt; its numbers are discarded, as are the four
runs taken on that node after the reboot, whose `dm` phase ran 3–4 × slow
while every digit stayed identical.

**The closing series** (`main` @ 3524146, job 20964 on s24-26, reference
evicted before every run, digits compared after each): **4 × 10¹⁰ in
80.20 / 82.16 / 80.58 / 81.36 / 78.95 s — wall 80.7 ± 1.2 s, phases
58.8 ± 0.2** (init 21.9 ± 1.1; bs 32.7 = batch 21.9 + top levels 10.6; dm 26.0 =
reciprocal 13.2 + division 12.8; T1/dc/T2 hidden), **peak host 12.1 GB**,
device 277 GB at init and 322 GB at the reciprocal's peak (the larger
planes), every run VERIFY OK and digits identical. Against Phase 11's
close (81.5 ± 1.4 s, phases 66.0): the phases are 7.2 s shorter — the
larger planes on the reserved-tail arena, the paired level 22 and the
fused inverse — while initialisation grew 6.8 s mapping them, so the
wall is level and the GPU work, which is what the multi-node run
multiplies, is 11 % less.

**576-node estimate (standing rule).** From `estimate.py` on the merged
code, with G's gridded profile and S's pool-resident slabs: **the
ceiling is ≈ 6.7 × 10¹⁰ digits per node — 3.9 × 10¹³ digits over 576
nodes in ≈ 4.0 minutes** of per-node wall (safe size 6.1 × 10¹⁰ per node
= 3.5 × 10¹³ in 3.8 min); G's own accounting of the gridded tree puts the
per-node ceiling at 7.1 × 10¹⁰ (4.1 × 10¹³ over the system), the
difference being the exchange scratch Q counts and G does not. Before
this session the same code fitted 9.5 × 10⁹ per node (5.5 × 10¹² total).
Labelled: per-node compute measured (4 × 10¹⁰ in 81 s, 8 × 10¹⁰ in 190 s,
10¹¹ in 263 s on one node); the fabric, the cross-node memory and the
SHMEM message cost modelled; the part-file bandwidth assumed.

## 78. Phase 13a — the first slice of the design-space campaign (2026-09-22/23; results/{P3,S13,K13,X13,M13,N13}.md)

Six agents under PLAN §30 (with its review corrections). The session ran long: an admin CI
array (`cdash-nightly`, 12 whole-node tasks at higher priority) held the PPAC nodes from
00:06 to about 03:30 EDT, s24-30 rebooted at 00:54 CDT and stayed down, s24-16 went to
another user, and an API session limit stopped all six agents from 01:10 to 03:50 EDT. The
agents' scripted batches ran unattended through the gap and nothing was lost. The target of
a real 2- and 3-node measurement (X) was not reached; everything else was.

**Merged tree** `int13` (M, S, K, P3, X, N; one conflict, the Makefile's TESTS list).
Defaults unchanged: every new path is behind a switch that is off.
Regression on the merged tree, s24-26: `--only unit,e9,mn,ckpt` **16/16** (job 21037),
`recheck` **2/2** (21038), `full` 4 × 10¹⁰ **identical, 80.90 s** and `stress` **10/10**
(21040). The `full` step's recheck printed RECHECK OK but was scored FAIL. Its criterion
still required P, Q from the top-level set, which Phase 12 turned off by default (3524146).
The criterion now accepts the sidecar (863a98f); rerun on job 21041: `full` identical at
81.15 s and its recheck **PASS** (P, Q from the sidecar, file read in 46.3 s). The whole
regression is therefore **21/21** on the merged tree.

**Closing series** (merged tree, job 21039, s24-26, reference evicted before every run):

| configuration | wall, 5 runs | mean | phases | bs | dm | init | device |
|---|---|---|---|---|---|---|---|
| defaults | 82.82 / 81.82 / 81.31 / 79.46 / 82.78 | **81.6 ± 1.4 s** | 59.1 ± 0.3 | 32.9 | 26.1 | 22.6 | 313.3 GB |
| `ECALC_NP=3` | 66.40 / 65.33 / 69.30 / 69.91 / 67.95 | **67.8 ± 1.9 s** | 47.0 ± 0.1 | 25.8 | 21.2 | 20.8 | 287.5 GB (P3) |

The defaults match Phase 12's close (80.7 ± 1.2, phases 58.8) within noise. Three primes cut
the phases by 20 % and the wall by 17 %. The series' own comparison looked for the reference
in the new clone, where it did not exist, and printed DIFFERS on every run (VERIFY OK on
all). `closing.sh` now finds the reference or stops. Two further `ECALC_NP=3` runs on the
merged tree (job 21041), compared against `results/e_4e10.out`, were **identical** (68.92 /
68.63 s). The default configuration's digits are covered by the `full` step, identical at
80.90 and 81.15 s.

### The options, measured (the Pareto table)

All wall times are 4 × 10¹⁰ at size 1 on one node unless stated. The 576-node ceiling is
modelled (`estimate.py --max` on the merged models, 502 GB per node).

| option (switch) | 4 × 10¹⁰ wall | node device memory | 576-node ceiling | label |
|---|---|---|---|---|
| **defaults** | 81.6 ± 1.4 s | 313.3 GB | 6.95 × 10¹⁰/node = **4.00 × 10¹³ in 4.3 min** | wall, memory measured; ceiling modelled |
| three primes `ECALC_NP=3` (P3) | **67.8 ± 1.9 s (−17 %)** | 287.5 GB (−25.8) | not modelled: `mn_model`/`mem_model` still assume four primes | measured |
| reduced-correction Barrett `NTT_MODMUL=1` (K) | phases 58.0 vs 58.4 s (one pair) | unchanged | unchanged | measured; transform +5–12 % |
| `MDB_SHIFT_CHUNK_MB=1024` (M) | no effect at size 1; +1 % at 10¹⁰/2, +13 % at 10¹⁰/4 | −4.9 GB/process of shift buffers at 10¹⁰/2 | 8.30 × 10¹⁰/node = **4.78 × 10¹³** | cost measured; ceiling modelled |
| + `MN_T_CHUNK_MB=1024` (M) | as above | −3.9 GB/process at 10¹⁰/2 | 9.24 × 10¹⁰/node = **5.32 × 10¹³** | cost measured; ceiling modelled |
| `ECALC_CKPT_TOP=2` (N) | 79.6 s with a 0.22 GB/s disk (set dropped); `=1` 128.2 s | Q held +17.8 GB during output when on | — | measured (disk emulated) |
| `NTT_MALL`, `NTT_B16_VAR`, non-temporal pack (K) | no gain (±2 %, pack up to 2.4 × slower) | — | — | measured; rejected |

### What the measurements decided

- **Three primes (E1/E3)**: exact to 2³³ points with margin 6.8 (27.2 at 2³¹); primes
  c = 240, 216, 207; CRT with 3 divisions per coefficient instead of 16. Planes fall
  180.4 → 154.6 GB, not 135.3: pool 1 holds one prime at a time and does not scale.
  The freed 25.8 GB is not yet turned into a higher ceiling (the sizing and models still
  assume four).
- **Strategy boundary (E0/E2)**: prime-per-APU (B) is the fastest dependent product at
  every size 2²⁰–2³²: 1.24–1.52 × four-step (C) to 2³¹, 3.7 × at 2³², where C must grid.
  At P = 3 its lead at 2³¹ narrows to 1.31 ×. **The boundary is memory**: B needs 16 n
  bytes per APU (32 GiB at 2³¹) against today's 28 GiB. Modelled at 4 × 10¹⁰, the big
  products take 32.2 s today, 25.0 s with B on 3·2ᵏ planes and 21.4 s with 4 GiB/APU more
  plane budget. E4 (B in the reciprocal) is justified; E5 (exchange-free grid) is marginal.
- **Kernel constants (H2/H3/H6/H7)**: no MALL cliff; the kernels are issue-bound at
  1.2–1.6 TB/s. The reduced-correction Barrett variant is bit-identical and 5–12 % faster
  per transform. Stride penalty found: s_lo 17 and 24 passes run at 0.73–0.82 ×, worth
  ~12 % of a 2³¹ transform, likely HBM aliasing. The b1 pass is the slowest everywhere.
- **The two fabrics (E9/H4)**: the xGMI push fills all three links at once (244 GB/s per
  APU, 93–97 %). Power-of-two node groups already hide 74–76 % of xGMI time under the
  fabric; non-power-of-two groups (3, 192, 576) hide 1.1 %, because the `alltoallv` path
  serializes. The overlap ceiling at 576 is 23.5 % of exchange time (not 27 %). Worth
  writing: a two-deep pipeline for `alltoallv`. Not worth writing: finer overlap, or the
  xGMI relief valve (per-APU traffic is balanced to 1.0000). Measured with several
  processes sharing one node only; the real 2- and 3-node runs are scripted (`~/x13/b5.sh`,
  `b3.sh`).
- **Memory truth (TASKS 1.1–1.3)**: one ceiling. The C request and `mem_model.py` agree to
  the byte at 35 points (new `BS_LAYOUT_ONLY`). Measured device totals are within 0.05 %
  at 10¹⁰, 4 × 10¹⁰, 8 × 10¹⁰ and 10¹¹. The earlier 6.7 vs 7.1 split was two versions of the
  model, not C against the model: Q's pre-G estimate of the top-product scratch, and 8.6 GB
  of SHMEM pool counted by only one version. The 6.7 in §77 and `docs/TARGET.md` is
  superseded by 6.95.
- **Hardening (TASKS 1.7, 4.1, 1.4)**: a restart under a different schedule or node count
  is refused on every node with exit 5 / 4. Before, three of four nodes segfaulted with
  wrong digits. Q is off the critical path. The top set at size > 1 is written in the
  background.

### 576-node estimate (standing rule)

**4.0 × 10¹³ digits in ≈ 4.3 minutes** of per-node wall on the defaults (6.95 × 10¹⁰ per
node, 502 GB; at the 480 GB safe budget 3.7 × 10¹³ in 3.9 min). With M's two chunking
switches on, the memory ceiling rises to 5.3 × 10¹³; its wall is not modelled. Three primes
would shorten the per-node compute by the measured 20 % of the phases, but that is not
modelled at 576. Labels: per-node compute measured; fabric, cross-node memory and SHMEM
message cost modelled; part-file bandwidth assumed.

### For the user to decide

`ECALC_NP=3` as the decimal default; `NTT_MODMUL=1` as the transform default; the two
chunking switches on for the 576 layout; `ECALC_CKPT_TOP=2` if the top set is ever enabled.
Each is measured above and none is adopted.

## 79. Phase 13b — the conclusive design table for the 576-node target (2026-09-23; results/{B13b,D13b,P13b,X13b,K13b}.md, results/DESIGN_TABLE.md)

**Step 0** (the user's approval at launch): three primes for decimal limbs and `NTT_MODMUL=1`
are the defaults (c988218; regression 16/16 on them). Five agents, then an integrator
measurement campaign (M-run: 68 completed runs on s24-26 and s24-30, 0 DIFFERS; full
comparisons against the references on the first run of each block, VERIFY on every run).
The merged tree passes the whole regression, **21/21** (jobs 21089 and 21088): the full
4 × 10¹⁰ run is identical at 69.2 s, and the recheck and 10 stress runs pass.

### What was built (all behind switches, off by default)

| switch | what | measured |
|---|---|---|
| `RNS_STRATEGY=C\|B\|B4\|auto` (B) | prime-per-APU in the library; auto = B where it fits the pools | auto −9.4 % of the 4 × 10¹⁰ phases at the same peak (B13b); M-run: auto 59.1 s at 3·2³⁰, the best one-node row |
| `ECALC_PLANE_CAP=2^30\|3*2^29\|2^31\|3*2^30\|fit` (P) | one switch for the plane cap; pool sizing follows the prime count | one-node ceilings at three primes: **1.44 × 10¹¹** (2³⁰), 1.30 × 10¹¹ (2³¹), 1.14 × 10¹¹ (3·2³⁰); the node fits while device + host ≤ ≈ 524 GB |
| `COMM_ALLTOALLV_DEPTH=2`, `COMM_LAYER_TKERNEL=1` (X) | the uneven exchange two deep; one transpose kernel | general map hidden 1.4 % → 74.3 % on **two real nodes**; size 3 at 10⁹: −7 % wall (M-run, n = 3 each); +33 % exchange scratch |
| `NTT_B1R=3 NTT_PLAN=1` (K) | register-blocked b1 pass; pass boundaries off the slow strides | transform 4–14 % (1.21–1.32 × at 2²⁵–2²⁶); M-run: auto 2³¹ 62.9 s (n = 8) against 65.4 s off (n = 3) = −3.8 % |
| `design_table.py` (D) | the 96-row table, the calibration and the M-run import | the gate passes: C/auto within 2 % wall / 0.1 % peak at all four caps |

### The table (576 nodes; `results/DESIGN_TABLE.md` has all 96 rows)

Per-node inputs are measured, and the 576-node columns are modelled from them. Fabric
bandwidth and per-message cost are assumed (100 GB/s per APU, PLAN §25). The chunk-round
cost T_ROUND is **assumed** at 0.03 s (range 0.01–0.1 s): the M-run's chunk sweep
(256/64/16 MB) showed no trend above the ±10 % noise of four processes on one node.

| row | design | max digits 502 / 480 GB | wall at 4 × 10¹³ (50 / 100 / 200 GB/s) | wall at its max | one node 4 × 10¹⁰ |
|---|---|---|---|---|---|
| baseline (row 8) | C, 2³¹, no chunking, depth 1: step 0 plus K's kernels, none of the new switches | 4.19 / 3.94 × 10¹³ | 4.37 / 3.70 / 3.37 min | 3.81 min | 64.5 s, 248 GB |
| **fastest** | auto, 2³¹, no chunking, depth 2 | 4.16 / 3.89 × 10¹³ | 4.26 / **3.59** / 3.26 min | 3.66 min | 62.9 s, 248 GB |
| **recommended** | auto, 2³¹, both chunkings, depth 2 | **5.52 / 5.19 × 10¹³** | 4.50 / **3.83** / 3.50 min | **6.30 min** | 62.9 s, 248 GB |
| **largest** | B4, 2³⁰, both chunkings, depth 1 | **6.40 / 6.04 × 10¹³** | 6.69 / 5.41 / 4.84 min | 12.2 min | 76.9 s, 197 GB |

The Pareto front has nine rows: from the fastest, through auto/2³¹ with one or both chunking
switches, then C/3·2²⁹/both at 6.08 × 10¹³, to the largest. The ranking is identical at 50,
100 and 200 GB/s. At 576 nodes the product strategy moves the wall by ≤ 0.01 min, because it
reaches only each node's own top levels. The plane cap sets the trade: 2³¹ is fastest, 2³⁰
largest. Chunking buys +1.4 × 10¹³ digits for +0.24 min at the assumed T_ROUND (the
recommended row takes 3.66–4.43 min over its range). The recommended row's environment:
`RNS_STRATEGY=auto ECALC_PLANE_CAP=2^31 MDB_SHIFT_CHUNK_MB=1024 MN_T_CHUNK_MB=1024
COMM_ALLTOALLV_DEPTH=2 NTT_B1R=3 NTT_PLAN=1`.

**What remains assumed, and is the target's first measurement** (`docs/TARGET.md` §6): the
fabric bandwidth and per-message cost; T_ROUND; the part-file rate; the target node's memory
edge (aac6: ≈ 524 GB). Depth 2's gain at 576 is a lower bound: the model hides only xGMI link
time, while X measured the host-side stage hidden too.

**M-run incidents**: the first 45-min block ran out of time during B4 at 3·2²⁹ (re-run, passed).
Three processes at 10¹⁰ on one node ran out of memory (their transform caches overfill a
shared node; replaced by 10⁹). A helper parsed multi-process memory and verdicts wrongly
(regenerated from the logs). Two process-kill slips by the integrator cost one run. No
digit was ever wrong.

### 576-node estimate (standing rule)

**Recommended design: 5.5 × 10¹³ digits in ≈ 6.3 minutes** at 502 GB per node (5.2 × 10¹³ at
the 480 GB safe budget). **Fastest design: 4.2 × 10¹³ digits in ≈ 3.7 minutes.** Both are
modelled from measured per-node inputs, with the fabric assumed. Against Phase 13a's
4.0 × 10¹³ in 4.3 min, the fastest design is quicker at a slightly larger size.

## 80. Phase 13c — the chosen design as the default; the target 4.4 × 10¹³ digits (2026-09-23)

**The user's choices** (from the design table, §79): the 480 GB-per-node regime. Design **auto, 2³¹, shift
chunking, depth 2**, with K's kernels. The **target is 4.4 × 10¹³ digits on 576 nodes**, chosen just below a grid
step (below). Defaults since commits 8f545fb and 54b541e:

| switch | default | earlier default / other values |
|---|---|---|
| `RNS_STRATEGY` | `auto` | `C` (was the default), `B`, `B4` |
| `ECALC_PLANE_CAP` | `2^31`, unless `POOL_LOG`, `RNS_PLANES_3Q30` or `DIST_LOGN_TEST` is given | `off` = the pre-13c size rule (3·2³⁰ below 5 × 10¹⁰ digits); the other caps; `fit` |
| `MDB_SHIFT_CHUNK_MB` | `1024` | `0` (one round, was the default) |
| `COMM_ALLTOALLV_DEPTH` | `2` | `1` (was the default) |
| `NTT_B1R`, `NTT_PLAN` | `3`, `1` | `0`, `0` (were the defaults) |

`MN_T_CHUNK_MB` stays off (the "shift" row). `estimate.py` defaults to the same design. Other fixes:
- `docs/TARGET.md` §4: the launch line passed 61000000000, the per-node share of the old safe size, but `ecalc`
  takes the total. It now passes 44000000000000.
- `docs/TARGET.md` §5: 4.4 × 10¹³ is the headline run.
- `docs/TARGET_TASKS.md`, new: the target-only tasks T1–T9, for the agent that works on the target.
- `TASKS.md`: a status section.

**The grid steps** (modelled, 576 nodes, 2³¹ cap). A tree level's products are cut into more pieces as they
outgrow the plane cap, and the wall jumps where the piece count does:

| total digits | pieces | wall step |
|---|---|---|
| 2.24 → 2.25 × 10¹³ | 93 → 109 | +12 % |
| 2.97 → 3.00 × 10¹³ | 124 → 148 | +13 % |
| 3.31 → 3.34 × 10¹³ | 153 → 167 | +7 % |
| 3.95 → 3.97 × 10¹³ | 185 → 201, in the reciprocal and division | +5 % |
| **4.435 → 4.464 × 10¹³** | **214 → 248** | **+12.6 % for +0.65 % digits** |
| 5.33 → 5.36 × 10¹³ | 283 → 315 | +8 % |

The design's 480 GB ceiling, 4.66 × 10¹³ in 272 s, is past a step. 4.4 × 10¹³ is 0.8 % below it: ≈ 234 s,
457 GB per node. The step positions follow the code's own splitting rule. They are not yet checked by a run
(TASKS status item 1).

**Checks on the new defaults** (commit 54b541e, aac6):
- **Regression 21/21.** Job 21097: unit, e9, mn, ckpt 16/16; 10⁹ identical in 12.8 s. Job 21098: recheck, full,
  stress 5/5; the full 4 × 10¹⁰ run identical at 64.2 s.
- **Five-run series, 4 × 10¹⁰** (job C13c, s24-26, the reference evicted before each run and compared after it):
  65.80 / 64.02 / 62.13 / 62.55 / 63.03 s, so **63.5 ± 1.5 s**. Phases 46.3 ± 0.2 (bs 23.7, dm 22.5); init
  17.2 ± 1.4. All five identical. Against Phase 13a's close (81.6 ± 1.4 s, phases 59.1) the wall is −22 % and the
  phases −22 %.
- **One node's share of the target, 7.64 × 10¹⁰ digits on one node** (s24-30): **137.9 s**, VERIFY OK.
  Device 340.7 GB + host 13.0 = 353.7 GB. The model gave 129.9 s and 355.6 GB: **time +6.2 %, memory −0.5 %**.
  The model is calibrated at 4 × 10¹⁰ and extrapolates to this size, so its per-node compute is low by about that
  much here.

### 576-node estimate (standing rule)

**4.4 × 10¹³ digits in ≈ 4.1 min** on the defaults, **≈ 460 GB per node**, inside the 480 GB budget. This is the
design table's 234 s (3.9 min) with its per-node compute, about 188 s of the 234, raised by the 6.2 % the share run
measured: ≈ 246 s. All modelled on measured per-node inputs. The fabric's bandwidth (100 GB/s per APU),
per-message cost and chunk-round cost are assumed until the target measures them (`docs/TARGET_TASKS.md` T1).

## 81. Phase 13d — the target's grid step, the model recalibrated, SHMEM on real nodes (2026-09-23; results/{L13d,D213d,G13d,S13d}.md)

Four agents under PLAN §32. **Regression 21/21 on the merged code** (commit 2270813: L's extractions and D2's
model; jobs 21115, 21116). The first attempt ran on `main` by mistake: the bundle assumed a commit aac6 did not
have. The script now stops if the clone is not at the expected commit.

### The target sits past two steps (L, D2)

**L, `MN_PLAN_ONLY=<total digits>:<g>`**: the C code's own decision functions (`mn_grid_shape`, `db_grid_shape`,
`grid_piece_skipped`, the Newton chain; extracted unchanged) print every large product's grid and pieces for any
size and node count. No GPU, 0.04 s. Validated against 9 real runs at 1–4 processes, including three with a lowered
cap so grids and cuts form. Every comparison agreed; unit and e9 steps 10/10.

**The step moved**: the code gives each node the same number of *terms*, so a node's digits grow with its term
index. At 576 nodes the top node holds **1.036 ×** the average digits (node 0: 0.77 ×). Every level waits for its
largest group, and the model had costed node 0's group at the average. The C code's plan at 576 nodes (critical
path pieces; `results/L13d_plan576.txt`):

| total digits | pieces | modelled wall (D2, recalibrated, 100 GB/s per APU assumed) |
|---|---|---|
| 4.25 × 10¹³ | 185 | **234 s (3.9 min)**, 452 GB per node |
| 4.29 × 10¹³ | 185 | 235 s |
| 4.30 × 10¹³ | 197 | 247 s |
| 4.34 × 10¹³ | 219 | 267 s |
| **4.40 × 10¹³** | 229 | **275 s (4.6 min)**, 463 GB per node |
| 4.40–4.74 × 10¹³ | 228–229 (flat) | — |
| 4.75 × 10¹³ | 236 | — |

**D2** fixed the model's structure to the C code: the term shares, the order of the k-way levels, the division's
A_h (dl + 1 limbs, not 2 dl + 1), the exact lengths, the size-1 leaf levels and auto's grid rules. Its piece counts
now equal L's at 401/401 sizes at 576 and 801/801 at size 1. The **6.2 % miss of §80** was the pipeline running its
big products slower than the isolated benchmark: a one-plane product 1.15 × (B form) / 1.22 × (C form), and
≈ 0.08 s per extra grid piece per 2³¹ limbs of the product. That per-piece cost is what makes the steps. The model
is within 3 % on 11 of 12 fitted one-node walls (5 × 10¹⁰–1.16 × 10¹¹); above that it is not gated.

### The steps on real hardware (G, s24-16, every run verified)

| predicted one-node step | below → above | Δ wall for Δ digits |
|---|---|---|
| 5.155 × 10¹⁰ | 79.2 → 83.4 s | +5 % for +1.4 % |
| 5.795 × 10¹⁰ | 94.5 → 98.8 s | +5 % for +1.2 % |
| 6.625 × 10¹⁰ | 109.8 → 121.0 s | **+10 % for +1 %** |
| 7.735 × 10¹⁰ | 136.4 (7.72) → 150.5 s (7.77) | **+10 % for +0.6 %** |
| 8.585 × 10¹⁰ | 172.6 → 178.9 s | +4 % for +0.6 % |
| 9.275 × 10¹⁰ | 189.8 → 191.7 s | +1 % (not seen) |
| 1.0305 × 10¹¹ | 225.1 → 237.3 s | +5 % for +0.7 % |
| 1.1595 × 10¹¹ | 307.9 → 313.9 s | +2 % for +0.6 % |

- **The mechanism at the target's own code path**: 4 processes, reduced cap, 1.26 → 1.31 × 10¹⁰ digits,
  110.9 → 132.7 s (+20 % for +4 %).
- **The 2³⁰ cap, 1.42 against 1.44 × 10¹¹, same node**: 1.047 × the wall, a moderate step, not P13b's 1.6 ×.
  P13b's 677 s run had every operation 2–3 × faster than its 1.44 × 10¹¹ run on another node: a node or
  memory-state effect. Strategy C at 1.42 × 10¹¹ / 2³⁰ took 1350 s here against auto's 1081 s.

**Two incidents on the defaults** (open, TASKS):
- **One hang after init at 7.70 × 10¹⁰**: 197 threads in `futex_wait_queue`, 2 in `kfd_wait_on_events`; no
  stack (ptrace_scope 1). Not reproduced in 4 more runs: 1 in about 30 runs this session.
- **Out of memory inside a phase at 1.245 × 10¹¹ on one node**: the pool could not grow by 27.7 GB with 41 GB free
  in pieces. At 1.252 × 10¹¹ the run passed. Above 10¹¹ on one node, auto forms grids of many small pieces because
  it ignores the per-piece cost (D2): dm 162–171 s at 1.16 × 10¹¹, where C took 90 s at 1.14 × 10¹¹ in P13b.
  Neither affects the 576-node target, whose large products are C-form, with a per-node share of 7.4 × 10¹⁰.

### SHMEM across real nodes (S; TASKS 1.5)

- **Sandia OpenSHMEM** (`~/sos`, libfabric sockets) works first time across nodes, in the target form (concurrent
  contexts, put-with-signal, device pool).
- **2 nodes**: `t_comm` in all 8 transport modes and `t_dist` layered VERIFY OK; ecalc 10⁸, 10⁹ and **10¹⁰ identical**
  over SOS and TCP, within 1.4 % of each other (10¹⁰: 2071 s against 2042 s; aac6's only inter-node link is 1 GbE).
- **3 nodes**: identical at both depths; SOS 36–47 % slower than TCP there (a guess, not measured: the provider's
  single progress thread for two peers).
- **Depth 2 at 3 real nodes**: 73–78 % of the xGMI time hidden under the fabric on the general-map node, against
  0 % at depth 1. This completes X13b's measurement.
- **OpenMPI 4.1 OSHMEM hangs across nodes**: a blocking put over UCX's tcp transport needs the target to progress,
  and its polled wait does not. It must not be used across nodes.
- **The default SHMEM pool is too small**: 8479 MiB in use at 10¹⁰ on 2 nodes, above the 8192 default;
  `COMM_SHMEM_POOL_MB=16384` was needed. The model's pool column is flat, so the pool model is open, and at the
  target it is `docs/TARGET_TASKS.md` T0.

### 576-node estimate (standing rule)

At the current target, **4.4 × 10¹³ digits: ≈ 4.6 min** (275 s), 463 GB per node. At **4.25 × 10¹³, just below
both steps: ≈ 3.9 min** (234 s), 452 GB per node. Modelled with the recalibrated model on measured per-node inputs;
the fabric (100 GB/s per APU, 2 µs per message) and the chunk-round cost are assumed. The SHMEM pool's size at the
target is not yet in the node total (T0). The choice between the two sizes is the user's.
