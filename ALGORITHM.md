# ALGORITHM — how ecalc computes e

Scope: the algorithm and data flow of ecalc, with size formulas and the source files and functions that implement each
part. | Maintained: updated in the same merge as any change to the algorithm or the code structure it describes.

This document describes the code on `main`. Environment switches (names, defaults, evidence) are in `ecalc/README.md`;
measured results are in `RESULTS.md`; objectives and status are in `PLAN.md`; open work is in `TASKS.md`; how to check a
change is in `TESTING.md`.

Source paths are relative to `ecalc/`. Numbers marked (m) are measured, (mod) modelled, (a) assumed.

## 1. Overview

`ecalc <digits> [outfile]` writes ⌊10^d · e⌋, the decimal digits of e = 2.71828…, with d = the requested digits rounded up
to a multiple of 18. The pipeline has four stages:

1. **Series.** e = 1 + P/Q with P/Q = Σ_{k=1..N} 1/k!, formed by binary splitting (§3).
2. **Reciprocal.** μ ≈ B^(n_Q+h)/Q by Newton iteration (§5).
3. **Division.** X = ⌊(P + Q) · B^dl / Q⌋ in two quotient halves (§5).
4. **Output and check.** X's limbs are the digits; they are written packed and verified by residues (§9, §10).

Everything is integer arithmetic on numbers held as **base-10¹⁸ limbs** (§2). All big products are number-theoretic
transform (NTT) products modulo three or four 52-bit primes, reconstructed by the Chinese remainder theorem (CRT)
straight back into decimal limbs (§4). One process per node drives the node's four APUs (MI300A, one HIP device
each); several nodes cooperate over a communicator stack (§6).

Notation used throughout:

| symbol | meaning |
|---|---|
| B | the limb base 10¹⁸ |
| d | digits computed: d = 18 · ⌈d_out/18⌉; `d_out` is what was asked for and what the ASCII file holds |
| dl | d / 18, the number of fractional limbs of X |
| N | number of series terms: the smallest m with log₁₀ m! ≥ d + 50 (`e_terms`, `binsplit.c`; bisection on `lgamma`) |
| n_Q | limbs of Q(1, N+1) = N!, about (d + 50)/18 |
| np | primes in a product: 3 or 4 |
| g | node-processes in a group; "size" is the node count of the run |

At d = 3.82 × 10¹³ the series has N ≈ 3.17 × 10¹² terms, and Q has n_Q ≈ 2.1 × 10¹² limbs (17 TB) spread over the nodes.

## 2. Number representation

**Decimal limbs.** An integer is an array of 64-bit words, each holding a base-10¹⁸ digit (< 2⁶⁰), least significant
first. The limbs of X, written from the top, *are* 18 decimal digits each, so there is no radix conversion at the end;
the price is that a limb carries 59.79 bits of a 64-bit word. There is no other limb base.

**Host numbers** (`bigint`, `bigint.c/h`): a malloc'd limb array; used for seeds, small values, tests and references.

**Device numbers** (`dbig`, `dbig.c/h`): one number held as four quarters, quarter d on APU d. Every operation (add, sub,
shift, compare, mod q) runs as one kernel per APU over that APU's quarter of the result and reads operands on any APU
through peer access; carries across chunks and quarters are resolved by a scan of per-chunk flags. A *view* addresses
limbs [lo, lo + len) of a number without copying; most cuts in the division are views.

**Sharded numbers** (`mdb`, `mdb.h`): a number of n limbs shared by a node group [g0, g0 + g). Node g0 + k holds the
contiguous share [N·k/g, N·(k+1)/g) of the sharding basis N ≥ n (the allocated length of the product that made it) in a
`dbig`. Every node of a larger group can describe the number from (n, N, g0, g) alone, so redistribution needs no
boundary exchange. P, Q and X are never gathered on one node.

## 3. The series and binary splitting

`binsplit.c`, `binsplit.h`; the node-level combine is `mn_tree` in `mn.c`.

**Recursion.** For a term range [a, b) define

    Q(a,b) = a (a+1) … (b-1),    P(a,b) = Σ_{k=a}^{b-1} (k+1) … (b-1),    so  P/Q = Σ_{k=a}^{b-1} 1/(a … k).

Two adjacent ranges combine as **P = P₁·Q₂ + P₂, Q = Q₁·Q₂**, and e = 1 + P(1, N+1)/Q(1, N+1) = (P + Q)/Q. The 50 guard
digits in N make the truncated series exact beyond d. Each combine is two products; the add of P₂ rides inside the CRT of
the first product as an "added operand".

**Seeds.** The term range is cut into *spans* of S terms; each span's (P, Q) is computed on the CPU by schoolbook
recurrence (`span`, `bi_span_step`). S is the largest span whose Q fits about 128 limbs (`bs_seed_terms_for`:
S ≈ 128·18/log₁₀ b, a few hundred terms). The spans are computed during device initialisation
(`binsplit_seeds_begin`) and streamed into the region arenas in chunks through pinned buffers, hidden behind the memory
mapping.

**Levels.** `binsplit_e` pairs adjacent nodes level by level (level-synchronous, bottom up). Each level places its nodes
in four *regions*, node i of n in region ⌊4i/n⌋, a subtree per region; a region lives in one APU's memory, so its products
are done by that APU with local reads. Two level pools of alternating parity hold the current and the next level. The tier
is chosen per level from the largest node length `max_nl`:

| condition | tier | what runs |
|---|---|---|
| 2·max_nl + 1 ≤ 2³⁰ points (`BS_MDEV_LOGL`) | **batch** | one `rns_mul_batch` of all 2·npairs products of the level (`rns_mul.c`) |
| otherwise | **device top levels** | each pair as `dbig` numbers through `rns_mul_dist_db` (`rns_dist.c`); inputs freed as consumed |

(The CPU schoolbook tier below `bs_school_nl` is set to 0: the batch tier is faster at every size. With `BS_BATCH_PIPE=1`, the default since D20, the batch tier's host side runs without needless gaps: a level's layout and the products' scans in parallel and each device's tiles issued back to back through pinned descriptor slots, one length read-back per level, `results/KERNELS.md §N3`; `BS_BATCH_PIPE=0` keeps the old host side.)

**Across nodes.** With size > 1, node r takes the term range [1 + N·r/size, 1 + N·(r+1)/size) and runs the whole leaf tree
on it, leaving (P_r, Q_r) on its devices. `mn_tree` then combines nodes in groups (§6.5), so after the last level P and Q
(the whole series) are sharded over all nodes.

**Reference.** `binsplit_ref` is the same recursion on the CPU with schoolbook products; tests compare against it, and
`ref/gen_e` produces GMP reference digits.

## 4. The product: RNS/NTT multiplication

All big products C = A·B (optionally + X, or restricted to a window of the result) are computed as integer polynomial
products: the limbs are the coefficients, the result coefficients are reconstructed exactly and their carries propagated
into limbs. Files: `modarith.h` (primes, modular arithmetic), `ntt.c/h`, `ntt3.c` (kernels), `crt.c/h`, `p24.h`,
`p24_crt.c`, `p24_mn.h` (reconstruction), `rns_mul.c/h` (batch tier, plane pools), `rns_dist.c`, `ntt_dist.c/h`
(distributed tiers, grids).

### 4.1 Primes and sizes

The primes are p = c·2⁴⁴ + 1 with 3 | c, c = 240, 216, 207, 147 (`ec_P` in `modarith.h`), all below 2⁵². The factor 3
gives roots of unity of order 3·2^k, so transform lengths are 2^k **or 3·2^k** (radix-3 layer in `ntt3.c`); a product pads
to at most 1.5× its coefficient count instead of 2×.

For operands of na and nb limbs the product has nc = na + nb coefficients; the transform length n is the smallest 2^k or
3·2^k ≥ nc (a cyclic convolution of length ≥ nc does not wrap). Each coefficient is at most min(na, nb)·(B − 1)². Three
primes give p₀p₁p₂ = 2^155.36 against (B − 1)² = 2^119.59, so three primes reconstruct exactly while
**min(na, nb) < 58 424 467 928** (`ec_np3_max_terms`). The prime count per product is `ec_np_for`:

- one-node tiers use three primes (`ECALC_NP` defaults to 3 in `ecalc.c`);
- multi-node products use `ECALC_NP=auto` (`ecalc.c` sets it when `ECALC_NP` is unset at size > 1): since the user's
  decision D18 (2026-10-12, `results/LAUNCH_DEFAULTS.md §DEF`) plane pool 0 holds four planes in every run of more than one node-process
  (`ec_np_planes`), so every multi-node product runs four primes in the P24 form (below). `ECALC_NP_MN3=1` keeps the
  three-prime multi-node path: three primes, or four (P24) when a piece of the run's largest group can exceed the
  three-prime bound (from 32 node-processes, which the target size needs at its top levels). `ec_np_check` refuses a
  product above the bound for its prime count.

`mn_plan.c` (`MN_PLAN_ONLY`) checks every planned product of a run against these bounds before any device is used.

### 4.2 Modular arithmetic and the NTT kernels

The modular multiply is FP64: the exact double product (hi, lo) by FMA, the quotient from a two-term reciprocal of p, and
one correction step ("reduced-correction Barrett", `mm_raw`/`mm_lazy`/`mm_canon` in `ntt.c`), giving values in [0, 2p) in
the butterflies and canonical values for twiddles. About 1.3 × 10¹² modmul/s per APU (m). Additions and subtractions are
64-bit integer, never double.

`ntt.c` implements a decimation-in-frequency forward transform and a decimation-in-time inverse, so no bit reversal is
needed: the forward output is bit-reversed and the pointwise product only needs both operands in the same order. A
transform of 2^logn points is a series of **passes**: register-blocked 7-stage radix-2 passes over the top stages, then one
pass on 2^10–2^12-point blocks in LDS; `plan_auto` moves pass boundaries off the slow strides at a few sizes. The pointwise
product is fused into the first inverse pass and the scaling by n⁻¹ into the last. Lengths 3·2^k (`ntt3.c`) do one radix-3
stage over the thirds and then three 2^k transforms (batch 3); the inverse mirrors it.

### 4.3 Reconstruction back to decimal limbs

After the inverse transform the product's coefficient k is known modulo each prime. **Garner's algorithm** (`crt.c`;
`dgarner3/4` in `rns_mul.c`) builds the exact coefficient c_k < p₀p₁p₂(p₃) (below 2²⁰⁶) and converts it to base-10¹⁸
digits (`ec_words_to_dec`, at most four). Limb k of the product is

    limb_k = C0[k] + C1[k-1] + C2[k-2] + C3[k-3] + x_k + carry,

where Cj are the j-th base-10¹⁸ digits of the coefficients and x_k the added operand; the carry chain is resolved in LDS
(per 256-coefficient chunk, as a carry-lookahead over the waves'
ballot masks, `results/KERNELS.md §K4b`; `RNS_CRT_SCAN=0` keeps the serial chain, D21).
On the GPU (`k_crt_batch`) a product's coefficients are cut into S stripes (a block per stripe, coalesced reads, 256
coefficients per chunk); each stripe emits a 4-limb *spill* past its end, and the spills are merged into the limbs
(`k_spill_merge`, or chunked-carry adds in the distributed tiers). A CPU Garner is kept for small products and tests.

**P24.** In multi-node four-prime products a transform point carries **24 digits** instead of 18 (`p24.h`): 4 limbs map
to 3 points, so a view of n limbs has ⌈3n/4⌉ points and a piece is 3/4 as long. A coefficient is then at most
min·(10²⁴ − 1)², which fits four primes while min(Pa, Pb) ≤ 10¹² (`P24_MIN_MAX`); the coefficient of point x, shifted by
s = 6·(x mod 3) digits, lands on limbs L(x) … L(x)+3 with L(x) = ⌊4x/3⌋. Gain: −38 % at 10¹⁰ digits on 2 processes (m;
`results/PRIMES_STRATEGY.md §P2415`). It applies to every multi-node product whose plane pool holds four primes (built in; since D18 every multi-node product; with `ECALC_NP_MN3=1` the 18-digit form below 32 node-processes).
Its CRT kernel `k_crt24` (`p24_crt.c`, one block per column, 256 points per chunk) sums the at most three points meeting
each of a chunk's up to 342 limbs and resolves the chunk's carry chain on one thread; with `P24_CRT_SCAN=1` (the default since D24, `=0` the serial chain;
`results/KERNELS.md §K4c`) as the same carry-lookahead over wave ballot masks as `RNS_CRT_SCAN` (two limbs per thread, 512-bit masks).

### 4.4 Product tiers

Every product goes to the cheapest tier that can hold it:

| tier | used for | how the four APUs share it | where |
|---|---|---|---|
| **batch** | the many small products of the lower binary-splitting levels | one APU transforms all primes of the products it owns (locality-aware, paired when two products share an operand), or the primes striped over a tile | `rns_mul_batch`, `rns_mul_batch_local` (`rns_mul.c`) |
| **dist, form B** | a node's large products when one prime's planes fit an APU | prime p on APU p, whole product per APU, no exchange; CRT by all four APUs reading the planes by peer access | `b_core` (`rns_dist.c`) |
| **dist, form C** | a node's large products otherwise | four-step over the node's xGMI: each prime's plane split in row blocks over the 4 APUs, 3 all-to-alls per prime | `dist_core` |
| **mn** | products over a group of g nodes (tree top, reciprocal, division) | four-step over 4g ranks, prime by prime, layered all-to-all (§6.3) | `mn_core`, `mn_grid` |

`RNS_STRATEGY=auto` (default) picks B where its planes fit the plane pools sized at init, else C, and never allocates.
The host product tier (`rns_mul`: one product per prime per APU from host operands, a Karatsuba split above the pool, a CPU
schoolbook below 2¹⁰ points) had no production caller and was removed (TREF, `results/CODE_REDUCTION.md §TREF`); the tests check products
against `ecalc/tests/tref.h` instead.
With three primes form B transforms on three APUs and the fourth only takes part in the CRT.

### 4.5 Plane cap and grids

The largest single transform is the **plane cap**: 2³¹ points per node by default (`ECALC_PLANE_CAP`: 2³⁰ or 2³¹; the
distributed tier's planes are powers of two, while form B and the batch tier keep their 3·2^k lengths). For g nodes
the cap is 2^(31 + ⌊log₂ g⌋) points (`mn_logn_cap`). A product with nc above the cap is cut into a **grid** of ka × kb
piece products:

- the pieces are views of the operands; the first writes straight into C, the others into a temporary window and a shifted
  add with carry (`mn_round_add`, rounds of `MN_T_CHUNK_MB`);
- the shape minimises the total transform points under a cost model that prices form B pieces at 0.70 (3·2^k
  form B lengths at 1.05 per point) and adds a per-piece pipeline cost (one search, `grid_search` in `rns_dist.c`, over a form per tier; one piece loop, `grid_loop`);
- the shape jumps at some sizes (a "grid step", e.g. about +20 s of modelled wall above 3.82 × 10¹³, mod;
  `results/GRID_GROUPS.md §S52`); the target size is chosen just below a step;
- **cuts** skip pieces that cannot contribute: `grid_piece_skipped(oa, ob, la, lb, lowcut, w)` skips a piece that starts at
  or above w (low products, the band of the reciprocal) or ends at or below `lowcut` (high products whose low limbs are
  discarded). Entry points: `rns_mul_dist_db`, `rns_mul_low_db` (A·B mod B^w), `rns_mul_high_db` (only C ≫ cut used),
  `rns_mul_band_db` (both cuts), and the `_mn` counterparts;
- a **transform cache** keeps the forward transform of an operand reused by several pieces (or by the reciprocal's last
  doubling and the division's X·Q), sized to the cap and counted in the memory budget (built in).

## 5. Reciprocal and division

`newton.c` (host reference), `newton_db.c` (device and sharded versions), `newton.h`. All big products are the tiers of
§4; k and h are limb counts.

### 5.1 Reciprocal (Newton, correction form)

The target is μ ≈ B^(n_Q + h)/Q to h limbs of precision (μ has h + 1 limbs), error a few units. The iteration doubles the
precision j → 2j in the **correction form**, with Q_t the top `take` = min(2j + 2, n_Q) limbs of Q and r the iterate:

    t = Q_t · r                    (a middle product: the band between a low cut and a high cut)
    u = t / B^(take − j)  ≈  B^(2j)
    d = |B^(2j) − u|, sign recorded
    corr = (r · d) / B^j           (a high product, pieces below j − guard skipped)
    r' = (r · B^j) ± corr

Two products per doubling. If the correction is larger than expected the step repeats at the same precision; an overshoot
(r' ≤ 0) shrinks r by 1/16 and retries. The chain of targets is **anchored from the top** (`newton_chain_next`: k, ⌈k/2⌉,
⌈k/4⌉, … read downward), so the last doubling lands exactly on k with no wasted precision. The seed is a schoolbook
reciprocal of Q's top limbs (`newton_seed_top`). Functions: `recip_db2` (one node, device), `recip_mn_body` (sharded).

**Error.** μ's unit scales with M = B^n_Q/Q (1 … B: decimal limbs are not normalised). A doubling j → jn < 2j cuts its own
error (≤ 2M + 3 units at 2j) by ≥ 1 limb; a doubling with jn = 2j (the anchored chain does this whenever the target is even)
keeps it, and a run of such doublings squares it (k = 2^m: up to two limbs lost, modelled). The division's corrections
absorb it unless the run is long and μ's top limbs are used uncut (DKM with h = k₁, i.e. an odd quotient length); then the
step-1 estimate can be off by more than the 64-correction limit (fatal, never wrong digits). `NEWTON_CHAIN_GUARD=1` (the default
since D25, `=0` the old chain) anchors the chain at k, ⌈k/2⌉ + 1, … so every doubling is cut: |error| ≤ 8 units at any M (results/DIVISION.md §RECIP).

### 5.2 Division in two quotient halves ("DKM")

With S = P + Q (formed in place in P's basis), A = S·B^dl and k = len(A) − n_Q + 1, the division X = ⌊A/Q⌋ is done as
GMP's `mu_div`: s = min(⌊k/2⌋, dl), k₁ = k − s, h = max(k₁, s + 1), so the reciprocal needs only h ≈ k/2 limbs (one
doubling fewer than a full-length reciprocal; −25.7 s at 10¹¹ digits, m). It is the only division, at every size and node
count: at one node P and Q always reach it as device numbers (`binsplit.c` hands a top level that ended on the batch tier
over to the block pool), and for k = 1, s = 0 and step 1 is the whole division. Several nodes need k_mu ≥ 5.

1. **Step 1.** X_hi = ((top limbs of A ≫ s) · μ_top) ≫ (k₁ + 1) limbs, a high product with the low pieces skipped (the
   estimate is low by at most the number of skipped pieces + 1). The remainder is computed modulo B^w, w = n_Q + 2:
   R₁ = (A ≫ s) − X_hi·Q mod B^w, from the low product X_hi·Q mod B^w; the true remainder is < Q < B^n_Q, so the window
   determines it. Corrections: while R₁ ≥ Q: R₁ −= Q, X_hi++; if negative: X_hi−−, R₁ += Q (at most 64, else a fatal
   error; usually 0–2). X_hi is exact and final here.
2. **Step 2.** The same for A₂ = R₁·B^s, giving X_lo and the final remainder R.
3. X = X_hi·B^s + X_lo. The product 10^d·(P + Q) is never formed: the division works on S with the shift dl.

Step-2 corrections that arrive after the writer has started on X (§9) are not applied to X but returned as a signed integer
`newton_x_dx`; the file's tail is patched.

Functions: `divmod_shifted_dkm`, `newton_db_divmod_shifted` (one node), `mn_divmod_dkm` (sharded, §6.6), with helpers
`dkm_est_db`, `dkm_corr_db` and the `mn_dkm_*` forms.

## 6. Multi-node distribution

### 6.1 Processes and ranks

One process per node, four OpenMP threads, one per APU (a process per APU was 3.7× slower at 8 nodes, m). `mn_init`
(`mn.c`) reads `COMM_RANK` and `COMM_SIZE` and opens the node's four **meshes**: mesh d connects APU thread d of every
node, rank = node. A transform over a group of g nodes uses nr = 4g ranks; rank ρ = g·d + r (APU d of node r).

### 6.2 The four-step transform

A transform of n = R × C points is arranged as a matrix with R rows and C columns; point m = i + R·j sits at row i,
column j. The **block-cyclic** ownership gives rank ρ the rows [R·ρ/nr, R·(ρ+1)/nr), so runs of consecutive limbs are
coalesced reads. A forward transform is: transforms along the rows, a twiddle multiply, **one all-to-all**, transforms
along the columns; a product needs three (A, B, inverse). The split is logR ≈ logn/2, kept within
[max(10, 5 + log₂ nr), logn − 10] (`mn_shape`). The slab pipeline cuts each exchange into `DIST_CHUNKS` = 8 chunks to
overlap transfer and compute. Group sizes that are not powers of two (3, 6, 192, 576, …) use the **general map**: rows
divided as evenly as possible and exchanged with `alltoallv` of per-pair slabs (`gen_fwd`, `gen_inv_pw` in `rns_dist.c`).

Each operand is redistributed once per product: node k packs, for every rank (r, d), the limbs of its share that lie on that
rank's rows (one contiguous segment, because a share is a contiguous limb range) and one `alltoallv` over mesh d delivers
them. The result's rows go back the same way, straight from the CRT output into the nodes' contiguous shares of C; the
CRT run spills are all-gathered (4 limbs per rank) and added by a fixed-length chunked-carry add whose carries across node
boundaries are resolved by a scan over (carry, propagate) flags (`mn_core`, `db_share_add_spills_x`).

### 6.3 The layered all-to-all

`comm_layered.c`. The rank space of a group transform is (4 APUs) × (g nodes). An all-to-all runs in four stages:

1. an **xGMI push** inside the node: APU d sends block d′ (the slabs bound for APUs d′ of every node) to APU d′;
2. a block **transpose** to node-major order;
3. one message per remote node over **mesh d** (APU thread d of this node to APU d of every other node);
4. a transpose back to global rank order.

Fabric bytes equal a flat all-to-all's; messages per APU fall from 4g − 1 to g − 1; the on-node quarter never touches a NIC.
Two exchanges are in flight (depth 2), so one exchange's xGMI stage hides under the previous one's fabric stage.
`alltoallv` has the same stages with per-peer counts (the node's send counts are all-gathered over the four APU threads).
All-gather runs the inter stage first and the xGMI stage second, so the fabric moves 4× less. `COMM_LAYER_INTER2` adds a
second inter communicator over the same PE set so that consecutive v-exchanges alternate, and `COMM_LAYER_VSLOT_*` take
the v-exchange slots from the communicator pool and share and release them between products (they are counted in the node
budget).

### 6.4 Transports

`comm.h` defines the communicator operations (`alltoall`, `alltoallv`, `allgather`, `allreduce`, `send/recv`, `sym_alloc`):

| file | transport |
|---|---|
| `comm_local.c` | one rank (size 1): no-ops and copies; the single-node pipeline runs through it |
| `comm_xgmi.c` | the node's four APUs: peer copies over the xGMI mesh, driven by four host threads |
| `comm_shmem.c` | OpenSHMEM: a communicator is a strided PE set; one symmetric pool holds every remote-writable buffer; the sender puts a slab at an offset the receiver chose, then a signal word; the receiver polls signals. Setup, collectives and the fallback data path |
| `comm_ofi.c` | libfabric (`cxi`): the device exchanges' data plane over every NIC of the node, one domain per APU thread, chunks striped over the NICs; completion is signalled through SHMEM. On by default wherever a cxi NIC is present; `COMM_OFI=0` forces the SHMEM path. ×2.1 over SHMEM at 10 nodes (m; `results/OFI_FABRIC.md §OFI17`) |
| `comm_layered.c` | the layered composition of an intra (xGMI) and an inter (SHMEM/OFI) communicator, §6.3 |
| `comm_sim4.c`, `comm_util.c` | a synthetic four-rank layout in one APU's memory for tests; shared helpers |
| `comm_copy.c` | N7D: the GPU copy kernel for copies that touch the SHMEM proxies' host-registered pool (`COMM_SHMEM_PX_KCOPY=1`, called from `comm_shmem.c`'s `COPY`) |

Multi-node builds need SHMEM (`make SHMEM_CRAY=1`; a plain build refuses multi-node runs). SHMEM, the meshes and the
communicators are opened before any HIP thread exists, because opening them after the arenas crashed in a vendor
initialisation (8 of 78 runs, m).

### 6.5 The tree

`mn_tree` (`mn.c`), schedule `mn_groups_parse` (`rns_dist.c`). Each node first computes its own 1/size of the terms (the
leaf tree, §3). The top levels combine nodes in groups following the `MN_GROUPS` schedule: by default the powers of two
that divide the size, then the odd part's prime factors in ascending order; at 576 nodes that is 2, 4, 8, 16, 32, 64, 192,
576 (two 3-way steps at the top). The children of level l are the groups of level l − 1.

- A **2-way level** (`tree_level`): P = P_A·Q_B + P_B and Q = Q_A·Q_B, two products over the whole group.
- A **k-way level** (`tree_level_k`): Horner from the top child down, (P, Q) = (P_{k−1}, Q_{k−1}); for i = k−2 … 0:
  P = P_i·Q + P, Q = Q_i·Q: 2(k−1) products in sequence, each balanced over all g nodes.
- Each product runs `rns_mul_dist_mn` → `mn_grid` → `mn_core` over the group. Dead shares are freed between a level's two
  products, so a node holds its child's P, Q, the running pair and the new pair.
- Uneven last groups are correct but not faster (`results/GRID_GROUPS.md §B06`); the group structure follows the rack layout.

The only checkpoint is the top-level set (`ECALC_CKPT_TOP`, `bs_ckpt_tree_*` / `bs_ckpt_bg_*` in `binsplit.c`): each node's P and Q after the last tree level, read by `ECALC_RECHECK`. There is no restart inside a run.

### 6.6 Sharded reciprocal and division

`recip_mn_body`, `mn_divmod_dkm` (`newton_db.c`). The reciprocal's low part is cheap: every node repeats the one-node chain
(`recip_db2`) up to the largest anchored target ≤ 2¹⁶ limbs (`newton_mn_chain_start`), so the first doublings need no
communication. The remaining doublings run on shares. For each step a cost model (`newton_mn_x1_level`) chooses the
**smallest prefix group** [0, 2^L) of nodes on which the step is cheaper, since small steps lose to latency on a large
group; r moves to a larger group (`x1_regroup`) when it grows, and Q_t is cut from Q by an exchange over the full group.
Window cuts are `mdb_shift` (arbitrary limb shift between shares, in rounds of `MDB_SHIFT_CHUNK_MB`), add/sub with the
cross-node carry scan (`mdb_addsub`), compare (`mdb_cmp`) and residues (`mdb_mod_qs`). The division's steps are those of
§5.2 on shares; X is never gathered.

**Gather.** Beyond descriptors and residues (a few words per node, `mn_allgather`) nothing is gathered in a normal run.
The T2 head tails (the 49 digits before each node's range) are all-gathered before the run.

## 7. The run, in order

`ecalc.c` (`main`). One process per node, four APU threads.

| # | phase | what runs | why |
|---|---|---|---|
| 0 | plan | `MN_PLAN_ONLY=<digits>:<nodes>` and `BS_LAYOUT_ONLY` print piece counts, the SHMEM pool and the node bytes without a device | the pool is sized before `shmem_init`; the layout is checked against the memory edges |
| 1 | transport | SHMEM, meshes, communicators open | before any HIP thread exists |
| 2 | pools | per APU: plane pools 0 and 1, then the region arena (HIP virtual memory, 2 GiB chunks) mapped in the background | nothing is allocated inside a later phase |
| 3 | seeds | spans computed on the CPU and pulled into the arenas | hidden under the mapping |
| 4 | self-tests | transport and layered-exchange self-tests (size > 1) | a broken transport fails in seconds |
| 5 | budget check | modelled peak against `ECALC_NODE_GB` (480) and MemAvailable, all-gathered; rc 8 if over | one node dying mid-run would hang the others |
| 6 | leaf | level-synchronous binary splitting of the node's terms (§3) | |
| 7 | tree | the `MN_GROUPS` levels as sharded products over node groups (§6.5) | P, Q stay sharded |
| 8 | reciprocal | μ to h limbs (§5.1, §6.6) | |
| 9 | S = P + Q | in place | memory |
| 10 | division | steps 1 and 2 (§5.2) | X stays sharded |
| 11 | output | each node writes its part file(s) from a background writer started during the division, packed, O_DIRECT (§9) | the write hides under the division |
| 12 | verify | T1 and T2, joined over the nodes; deferred corrections patched (§10) | no reference exists at the target size |

At size 1 the same flow runs without the tree, with the dist tier in place of the mn tier.

## 8. Memory management

Device memory is the limiting resource (373.44 GB per node on the target, 93.36 GB per APU, m; host budget 480 GB), so
everything is allocated once, up front, and sized by a model that the C layout reproduces exactly (`mem_model.py --check-c`).

- **Plane pools** (`rns_init`, `rns_plane_pool_bytes`): per APU, pool 0 holds np planes of q limbs and pool 1 holds
  3q + 16, with q = 2^(pool_log − 2). Planes are mapped first because the last
  allocations near the edge get small pages. A plane pool does not grow inside a phase unless `RNS_POOL_GROW` is set;
  growth is counted and otherwise an error (rc 6). An unused pool tail is donated to the block pool.
- **Region arena → block pool.** Per APU one HIP virtual-memory range (`db_vmm_arena_alloc`). Binary splitting places each
  level's numbers in regions of two alternating parities (`pool_get`, `donate_pools`); after the leaf levels the arena
  becomes the `dbig` **block pool** of the reciprocal and division (`db_pool_*`, `dbig.c`). A request with no contiguous
  free extent is met by remapping free chunks, not by copying. The arena's tail is reserved for the division's largest
  block (the "hole", `dm_layout`) plus a room of 0.16 × the hole. The reciprocal's scratch is reserved once at the last
  doubling's sizes and inputs are freed as consumed (`DM_TIGHT`), which keeps the free space contiguous.
- **Window rounds.** A grid piece's temporary window is processed in rounds of `MN_T_CHUNK_MB` per APU (code default 1024,
  launch lines 2048); `mdb_shift` slabs are bounded by `MDB_SHIFT_CHUNK_MB`.
- **Exchange memory.** Staging for SHMEM/OFI and the general map's v-slots are counted in the node budget
  (`binsplit_vslot_bytes`, `binsplit_shmem_pool_need`); `COMM_LAYER_VSLOT_*` share and release them in time.
- **Memory fit (FIT, `results/LAUNCH_DEFAULTS.md §FIT`).** `binsplit_fit` (`binsplit.c`) puts the node's device peak -- the larger of the dm
  phase (planes + arena + comm_ofi pools + the v-slots) and the bs phase (the same + the VMM pool's bs growth, 6.0 GB m, + the
  tree top's v-slots) -- against the device edge `ECALC_DEVICE_GB` and the node (planes + arena + bs growth + host + v-slots)
  against `ECALC_NODE_GB`, at rank 0's layout, so every rank computes the same numbers. `MN_PLAN_ONLY` prints it (`plan fit`),
  a run prints it (`memory fit:`), `BS_LAYOUT_ONLY` prints a `fit:` line that `mem_model.py --check-c` compares; the launchers
  refuse a run that does not fit. With `DM_MN_WRAP` unset the same function decides the division's cyclic-plane low product:
  on only when the device with its two extra numbers stays within `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB` (5 GB).
- **Budget and guard.** `budget_check` (before compute) compares the modelled peak with 480 GB and MemAvailable (rc 8).
  During the run `ECALC_MEM_GUARD_GB` stops with rc 9 when MemAvailable falls below it.
- **Layout and models.** `binsplit.c` (`dm_layout`, `layout_arena`, `binsplit_node_bytes`, `tree_need_dev`) computes the
  layout; `BS_LAYOUT_ONLY` and `MN_PLAN_ONLY` print it without a device; `mem_model.py` mirrors it. Measured layouts agree
  with the plan within 0.6 GB (335.6 vs 335.0 GB at 4 nodes, m).
- **Pinned host memory** (`mem.c`): staging and level pools are first-touched by threads pinned to the APU's NUMA node (the
  wrong node costs 55×) and registered with HIP.

## 9. Output

`mn_out.c/h`, `packed_fmt.h`, `spill.c`, `tools/unpack_digits`.

X < 10^(d+1) has nl = ⌈(d + 1)/18⌉ limbs; the digit string is the limbs written from the top, 18 digits each, with
pad = 18·nl − (d + 1) leading zeros dropped; digit 0 is the "2" and digits 1…d the fraction. The ASCII file is "2." +
digits 1…d_out + newline.

**Packed form.** The default output is **packed** (`ECALC_OUT_PACKED=2` since 2026-10-12): each node writes its limbs as a
60-bit little-endian stream, 15 bytes per two limbs (0.4167 B/digit), most significant limb first, behind a 4096-byte `ECPACK18` header giving the part's limb and digit range,
the T1 primes and the part's digit residues. No ASCII formatting happens in the run; `tools/unpack_digits` makes the ASCII
file off the clock (`--cmp` compares without writing). `ECALC_OUT_PACKED=1` writes 8 bytes per limb (0.444 B/digit, the
default before 2026-10-12), `=0` ASCII.

**Parts.** At size 1 the file is `<outfile>`. At size > 1 each node writes `<outfile>.part<k>`, k = size − 1 − rank (part 0
holds the leading digits), so the sorted parts are the file; by default (`MN_OUT_DKM_HI=1`) each node writes two parts (its share of
X_hi and of X_lo; nparts = 2·size). The converter takes the set in any order.

**Early writer.** The writer is a background thread started by the division: right after step 1 on X_hi (final: step 2 changes only limbs below s) and later on
X_lo (`MN_OUT_DKM_HI=0` starts it at the division hook instead: X formed, before the low product). Writes use O_DIRECT through aligned buffers (`ECALC_ODIRECT=auto` chooses by file system). Division **corrections**
that arrive after the writer started are not applied to X; the output stage patches only the tail the carry or borrow
reaches (`mn_out_tail_fix`): it rewrites the 4 KiB blocks holding the changed limbs and moves the header's residues by the
same change, so the file stays consistent.

## 10. Verification

A run is good only with `VERIFY OK` on every node (`verify.c/h`, `out_stage` in `ecalc.c`).

- **T1.** Eight fixed 62-bit primes q_i (the first primes above 2⁶²). Each node computes P and Q modulo q_i by the series
  recurrence over its own term range, in the background during its leaf levels (`pq_bg_start`, `vf_pq_range_mod`), and the
  results are joined over the nodes (`vf_pq_join`). The residues of the actual limbs of P, Q, X and the remainder R (each
  share placed at its offset, `mn_out_res_share`, `vf_shift_res`) are checked against the identity
  (P + Q)·10^d ≡ X·Q + R (mod q_i) and against the recurrence's P, Q. This catches any wrong limb in binary splitting,
  reciprocal or division. The digit string's residue must also equal X's, which checks the written bytes (computed from the
  packed bytes themselves).
- **T2.** 50-digit windows of the output at fixed offsets against known digits (`tier2_range`); a window straddling a chunk
  or node boundary is checked by the piece it ends in, from the 49 digits before it. Windows in the patch zone are checked
  after the patch.
- **RECHECK.** `ECALC_RECHECK=1` re-verifies finished files from the `<out>.t1` sidecar (and the top-level P, Q set when
  written with `ECALC_CKPT_TOP=1`) without recomputation; it detects a single flipped digit.
- **Off the clock.** `tools/unpack_digits` checks every packed part against its header residues while converting;
  `ecalc/digcmp.sh` compares with a GMP reference (references exist up to 10¹¹ digits). The checks a change must pass are
  in `TESTING.md`.

## 11. Models and planning tools

| tool | what |
|---|---|
| `estimate.py` | wall time by phase for a size, node count and fabric profile; `--target` prints the standing estimate |
| `mn_model.py` | the cost model behind it (all-to-all rate per APU thread, hiding fractions, round costs) |
| `mem_model.py` | the memory layout per node; `--check-c` compares with the C layout |
| `calib_table.py` | run logs → one row per run; `--fit`, `--ab` (rate-adjusted paired A/B) |
| `tools/groups_table.py` | `MN_GROUPS` schedules and node counts at a fixed size |
| `MN_PLAN_ONLY`, `BS_LAYOUT_ONLY` | the code's own decisions (grids, pieces, prime counts, layout, the memory fit) without a device |
| `tools/plan_values.py` | the launch values of a size; `--fit`: the memory-fit report, exit 1 when a run does not fit |

Fabric profiles: `target_l4` (default; the target fabric with the L4 refit, `results/TIME_MODEL.md §L4`) and `aac7` (the S48 refit,
`results/TIME_MODEL.md §S48`). The model is calibrated on the test system up to 6 nodes; the target's per-NIC rate and its fall-off are
assumed (a).

## Glossary

| term | meaning |
|---|---|
| APU / node / node-process | one MI300A is one APU and one HIP device; a node has 4 APUs on an xGMI mesh; one node-process drives them |
| B | the limb base 10¹⁸ |
| batch tier | the product tier of the lower binary-splitting levels: many small products in one launch |
| binary splitting | the recursion P = P₁Q₂ + P₂, Q = Q₁Q₂ over term ranges |
| block pool | the per-APU virtual-memory arena from which `dbig` numbers are carved after the leaf levels |
| B / C (forms) | the one-node product forms: prime per APU without exchange (B), four-step over xGMI (C) |
| cut | a skipped part of a grid: pieces below a low cut or above a high cut of the result |
| dbig / mdb | a device number in four quarters / a number sharded over a node group |
| DKM | the division in two quotient halves with a half-length reciprocal (GMP's `mu_div`) |
| four-step / general map | the distributed transform n = R × C with one all-to-all per transform; the general map serves group sizes that are not powers of two |
| Garner | the mixed-radix CRT reconstruction of a coefficient from its residues |
| grid / pieces / plane cap | a product above the largest single transform is cut into ka × kb piece products |
| layered exchange / mesh d | the multi-node all-to-all in xGMI and fabric stages; mesh d links APU d of every node |
| limb | one base-10¹⁸ digit in a 64-bit word |
| MN_GROUPS | the schedule of group sizes of the tree levels |
| NTT | number-theoretic transform: a Fourier transform modulo a prime |
| P24 | 24 digits per transform point in four-prime products |
| packed | part files of raw 10¹⁸ limbs behind an `ECPACK18` header with the part's residues |
| plane | one prime's residues of one operand, 8 bytes per point |
| region / arena | the per-APU part of a binary-splitting level / the per-APU virtual-memory range holding the regions |
| seeds / span | the leaf (P, Q) of a few hundred terms each, computed on the CPU during init |
| T1 / T2 / RECHECK | residue check modulo eight 62-bit primes / digit windows / re-verification of finished files |
| v-slots | the general map's exchange buffers, counted in the node budget |
| X2 | the layered exchange with two inter-node stages and pooled slots |
