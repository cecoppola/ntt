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

Open trade-offs (sized, not chosen): a 3·2³⁰-point plane pool for the
dist tier (+40 GB of device pools, removes the second split of the
4.4 × 10⁹-limb products, ≈ −5 s per such product); Karatsuba instead of the
2×2 split in the device tier (¾ of the work, one more temporary).
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
The mdev-level (host pool) path is implemented but only exercised at
10¹⁰ and above: to be run once before relying on it (recipe in WP7.md §4).
For the multi-node run the same snapshot per rank, at the level boundaries
that are global synchronisation points anyway, is the restart design.
