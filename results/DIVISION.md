# DIVISION — the distributed division and the reciprocal

Scope: the distributed division and the reciprocal: Newton error, DKM halves, middle product, band-sized products, the low product, rebalance. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A-div | M4: the distributed division, with B2/B3/C3 (Phase 9, PLAN.md §19) |
| P1 | the distributed division multiply: the low product X·Q as a low part + one cyclic plane (`DM_MN_WRAP`) |
| RECIP | the Newton reciprocal's error: the t_newton bound or the reciprocal? |
| R415 | R4 (Phase 15, row 1b): the reciprocal's Q_t·r as a middle product (V3 item 4 = PLAN §33 B2) |
| R114 | R1 (Phase 14, W1): E7 the reciprocal's products cut to the band read; E9 Karatsuba over the grid (model); E10 the 576 tr |
| DKM15 | the division in two quotient halves with a half-length reciprocal (Phase 15 Batch 3, agent DKM) |
| EW15 | the writer starts on X_hi's digits after DKM's step 1 (Phase 15 Batch 3, agent EW) |
| K15 | a correction without a rewrite (PLAN §36, row 1d) |
| M615 | Phase 15 Batch 2, agent M6: item 6 (E11, band-sized products and the chunked division window) with the Q/S margin trim |
| O2 | the reciprocal-gather stall: why `db_to_bi` of 0.2 MB takes 2.2 s (2026-10-11/12) |
| O5 | K7-a GPU-3 rebalance -- the B5 form (B4 with APU 3 given a share of the inverse) |

## A-div — M4: the distributed division, with B2/B3/C3 (Phase 9, PLAN.md §19)

(former file: A-div.md)

Branch `adiv-m4` (from `main` @ a75474d). Files owned: `ecalc/newton_db.c`, the dm flow of
`ecalc/ecalc.c`. Touched outside (minimal, commented): `ecalc/dbig.c/.h` (two fixed-length share
ops), `ecalc/newton.h` (one declaration), `ecalc/mdb.h` (a struct tag only), `ecalc/Makefile`
(dependencies of `newton_db.o`). `rns_dist.c`/`mdb.h` interfaces unchanged (A-grid's).

#### Design

After `mn_tree` the final P, Q are `mdb` over the top-level group G = [0, size). The dm phase now
runs over G (`newton_mn_divmod`, default; `MN_DM=host` keeps M3's gather-to-node-0 flow); X stays
an `mdb` and is gathered to node 0 (`mn_gather_host`) for the existing output until A-out lands.
The residues of P, Q and R mod the eight T1 primes come from the sharded kernel. `size` 1 goes
through none of this (bit-for-bit: `mn_init` returns 1).

Every big product is `rns_mul_dist_mn` over G, unchanged. Everything else is one of a few
share-level primitives in `newton_db.c`:

| primitive | what | communication |
|---|---|---|
| `mdb_shift(Y, X, s, N2)` | Y = X >> s (s < 0: <<) in a chosen sharding basis N2; truncates to N2 limbs (mod B^N2) when asked | one all-to-all per APU thread over the group's mesh d: a piece = (source share, target share) intersection, APU d carries its quarter of every piece (`k_mn_pack` reads any quarter by peer access, `k_mn_scatter` writes the target share); slab = the longest piece quarter, padded; a truncated result is re-normalised by `allreduce_max` |
| `mdb_addsub(Y, A, B, sub)` | Y = A ± B, one basis (in place allowed) | the fixed-length share add (`db_share_addsub`: `addsub_core2` with the (carry, propagate) flags) then the node scan (an all-gather of one byte per node, cin_r = c_{r−1} \| (p_{r−1} & cin_{r−1})), a second pass `db_share_add_val(…, 1)` on the nodes receiving a carry/borrow; the top node's carry-out aborts (the bases are chosen so it cannot happen) |
| `mdb_add_val(Y, pos, v, sub)` | Y ± v at limb pos (the X corrections) | the same scan (every node runs the kernel with v = 0 elsewhere, so every share's propagate flag is known) |
| `mdb_cmp`, `mdb_limb`, `mdb_nonzero_below`, `mdb_norm`, `mdb_pow` | per-share values combined | one all-gather / max-reduction each |
| `mdb_mod_qs` | residues mod ≤ 16 primes | each share's `db_mod_qs` scaled by B^lo mod q, all-gathered and summed |
| `mdb_to_host_all`, `mdb_from_db/bi` | small numbers replicated on every host (Q's top for the seed chain; the rare overshoot shrink) | an all-to-all of g copies over mesh 0 |

**The reciprocal (`recip_mn`).** The anchored doubling chain from k (k, ⌈k/2⌉, ⌈k/4⌉, …) is the
single-node one. The chain's targets up to the largest one ≤ `NEWTON_MN_SPLIT` (2¹⁶ limbs) are
computed on *every node identically* with the single-node `recip_db` on the top T = 2 kp + 2
limbs of Q (one `mdb_shift` + all-to-host; the iterates at precision j read only the top 2j + 2
limbs of Q and the seed is scaled by the real n_Q, so they equal the single-node ones limb for
limb). Each node then takes its share of r (`mdb_from_db`, no exchange) and the remaining steps
are the correction-form step of `recip_db` over shares: Q_t = Q >> (n_Q − take) (a shift), t = Q_t r
(product), u = t >> (take − j) in basis 2j + 2, d = |B^{2j} − u| (`mdb_pow` and a sub), corr = (r d)
>> j and r << j in one basis NB = max(r.n + j, …) + 2, r' = r << j ∓ corr, the repeat/overshoot
logic as in `recip_db` (the overshoot shrink r −= r/16 through the hosts, rare). Per step: 2
products, ≈ 6 shifts, 2 adds, a few tiny all-gathers.

**The division.** Mirrors `newton_db_divmod_shifted`: the residues of P and Q first; the
reciprocal μ with k_μ from S's largest possible length; S = P + Q in P's basis (Q re-sharded into
it by `mdb_shift(…, 0, P.N)`; P.N = n_a + n_b + 1 > P.n so the sum fits); μ's top k + 1 limbs; A_h =
S >> (n_Q − 1 − dl) (a shift); t = A_h μ; X = t >> (k + 1); the low product X Q as the full product
truncated to w = n_Q + 2 (`mdb_shift(…, 0, w)`); the window (S mod B^{w−dl}) B^{dl} as `mdb_shift(S,
−dl, w)` (the truncation clips the pieces); Q in basis w; the ±Q corrections with `mdb_cmp` /
`mdb_addsub`; X ± dx by `mdb_add_val`; R's residues by `mdb_mod_qs`.

**B3 (single node)** `mul_high_db` in `newton_db.c`: the A_h μ product as the grid of piece
products (the split of `mul_grid`: the fewest plane points), skipping every piece whose limbs
end at or below the cut k + 1 (each is < B^{k+1}: X low by at most their number + 1, absorbed by
the up-corrections). Products that fit one plane (10⁸, 10⁹) are untouched; at 4 × 10¹⁰ the 2 × 3
grid loses its (0, 0) piece (one 2³¹-point product of six). `NEWTON_HIGHPROD=0` restores the
full product. Over shares the product is still one piece (no grid split over shares yet); the
skip belongs in A-grid's grid, see below.

**C3** `ECALC_DM_POOL=1`: before the reciprocal the block pool of every device is compared with
the reciprocal's scratch (r, r2: k + 4 each; t1: max(n_Q + k, 2k) + 8; the grid's piece temporary
2³¹ + 8; per device a quarter, plus 1 GiB) and grown once by the deficit (`db_pregrow`) instead of
block by block inside the phase. Off by default (see the timing below).

**B2 (not implemented — the tier's API does not allow it).** The last doubling's Q_t r (take =
n_Q at the top, so Q_t = Q) and the division's X Q share the operand Q and, at 4 × 10¹⁰, the same
piece boundaries (split_grid gives Q 3 pieces in both: 3 × 1 for Q × r with r ≈ k/2, 2 × 3 for
X × Q). `dist_core` forwards B one prime at a time into a single plane, so a cached transform of
Q's pieces needs, per piece, four planes per APU kept across A_h μ: 4 primes × 2²⁹ × 8 B = 16 GiB
per APU per piece, 48 GiB per APU for the three pieces, on top of the phase's ≈ 62 GB per APU —
feasible only with the block pool sized for it, and only for the single-node path. The change
A-grid would need in `rns_dist.c`: (1) `mul_grid`/`dist_core` take an optional *transform cache*
argument `{ka, kb, piece length, per-piece per-prime plane pointers}`; `dist_core` with a cache
hit skips `k_gather` + `dist_fwd` of B for that prime and reads the cached plane (the pointwise
multiply then writes into xa[p] as now); a miss forwards into the cache's plane instead of xb;
(2) `rns_mul_dist_db` gets a variant `rns_mul_dist_db_cached(C, A, B, cache)`; the caller
(`recip_db` at its last step, `newton_db_divmod_shifted` for X Q) passes the same cache object
when B is the same dbig with the same piece length; (3) the cache's planes come from
`db_pool_alloc` (they must survive the block pool's frees between the two products). Expected
gain (PLAN I4): −3…−5 s of dm at 4 × 10¹⁰. Over shares the same applies to `rns_mul_dist_mn`
once the grid over shares exists (the pieces are views of shares with a global offset).

**Interface notes for A-grid (the grid over shares).** For the sharded division the two cuts
should be parameters of the grid: `lowcut` (skip pieces with oa + ob + len_a + len_b ≤ lowcut: the
A_h μ product, B3) and `highcut` (skip pieces with oa + ob ≥ highcut: the low product X Q mod B^w,
as `rns_mul_low_db` does on one node). `newton_mn_divmod` calls `mn_prod(&t, &Ah, &mu, G)` and
`mn_prod(&xq, &Xn, Q, G)` at the two places marked; with a signature such as
`rns_mul_dist_mn_cut(C, A, B, X, G, lowcut, highcut)` those two calls are the whole change.

#### Tests

Build in `~/ntt-adiv` (aac6) from the branch bundle. Every run: `VERIFY OK` and the written digits
byte-identical (`cmp`) to `~/ntt/ecalc/ref/e_<digits>.txt` / `results/e_4e10.out`. Commands:
`srun --jobid=$J -N1 --gpus=4 bash -lc "module load rocm; cd ~/ntt-adiv/ecalc; env POOL_LOG=27 ./ecalc 100000000 out"`
(size 1) and `SLURM_JOB_ID=$J ./mnrun.sh <procs> env POOL_LOG=27 [BS_MDEV_LOGL=21] ./ecalc 100000000 out`
(scripts `adivtest.sh`, `batch1/2/3.sh` in the home directory on aac6; outputs in
`~/ntt-adiv/ecalc/results/adiv/`). Walls are of the whole run (TCP fabric: correctness only).

##### One node, several node-processes (job 20734, s24-26)

| digits | POOL_LOG | node-processes | extra env | digits | wall | dm over the nodes (reciprocal) |
|---|---|---|---|---|---|---|
| 10⁸ | 27 | 1 | | identical (the single-process path) | 6.0 s | — |
| 10⁸ | 27 | 2 | | identical | 9.8 s | 2.25 s (1.40) |
| 10⁸ | 27 | 4 | | identical | 9.1 s | 1.54 s (1.10) |
| 10⁸ | 27 | 2 | `BS_MDEV_LOGL=21` (leaf P, Q on device) | identical | 9.1 s | 3.46 s (2.61) |
| 10⁸ | 27 | 3 (gt = 2 at the top) | | identical | 10.4 s | 2.46 s (1.52) |
| 10⁹ | 29 | 1 | | identical | 17.9 s | — |
| 10⁹ | 29 | 2 | | identical | 32.4 s | 10.38 s (7.20) |
| 10⁹ | 29 | 4 | `BS_MDEV_LOGL=25` (the device top levels) | identical | 28.6 s | 6.44 s (4.53) |

At 10⁹ over 4 nodes: the single-node chain to 54 254 limbs (0.13 s), 10 sharded steps to
55 555 558 limbs; 52 shifts 1.6 s, 22 adds 0.07 s, products 3.2 s (reciprocal) + 1.4 s (division);
0 corrections in every run (as on one node). T1 (P, Q, R residues from the sharded kernel, X on
node 0) and T2 pass everywhere.

##### Two real nodes (job 20735, s24-16 + s24-26)

| digits | node-processes (placement) | extra env | digits | wall | dm |
|---|---|---|---|---|---|
| 10⁸ | 2 (one per node) | | identical | 43.0 s | 20.0 s |
| 10⁸ | 4 (two per node) | `BS_MDEV_LOGL=21` | identical | 39.6 s | 25.9 s |
| 10⁹ | 2 (one per node) | `BS_MDEV_LOGL=25` | identical | 245 s | 189 s (the 1 GbE fabric: 97 s of products, 29 s of shifts) |

##### 4 × 10¹⁰, size 1, the timing gate (job 20734, s24-26, reference evicted from the page cache first)

| run | wall | dm (reciprocal) | digits |
|---|---:|---:|---|
| baseline (RESULTS §72) | 98.5 s | 37.6 (15.8) | — |
| this branch, default (B3 on: 2 × 3 pieces, 1 skipped) | **91.2 s** | 32.8 (15.1) | identical, VERIFY OK, 70.8 GB |
| `NEWTON_HIGHPROD=0` (B3 off) | 94.3 s | 33.6 (14.8) | identical |
| `ECALC_DM_POOL=1` (C3) | 93.8 s | 32.2 (15.0) | identical; the pool already covered the 23.1 GB per device: grown by 0 |

Not slower than the baseline (single runs, unshared node; the 3 s between the runs is the usual
init/bs variance). B3 takes 0.8 s off the division at 4 × 10¹⁰ (one 2³¹-point product of six);
C3 changes nothing at 4 × 10¹⁰ (the estimate is below the donated regions there) and was not
measured at 6–7 × 10¹⁰ (no node time for a 3-minute run in the batches); it stays off by default
until that measurement.

#### Open issues

- The all-to-all buffers of `mdb_shift` are g × slab per APU (the pattern of M3's redistribution):
  fine to hundreds of nodes, an all-to-all-v would remove the padding.
- `mdb_to_host_all` replicates a small number on every host; it is only used for Q's top 2 kp + 2
  limbs and the (never observed) overshoot shrink.
- X is gathered to node 0 for the output (A-out replaces this).
- The low product over shares is the full X Q; the skip of the pieces above w and B3's skip need
  A-grid's grid over shares (parameters described above).

## P1 — the distributed division multiply: the low product X·Q as a low part + one cyclic plane (`DM_MN_WRAP`)

(former file: P1.md)

Labels: **m** measured, **mod** modelled, **a** assumed. Times Eastern. Branch `p1` (from `main` b60e34d).

**Question.** Where does the multi-node division (dm) spend its time, and can the division's multiply be made cheaper without
changing the digits? PLAN §2: dm is the largest multi-node cost (+94 s at 6 nodes against 1, m).

#### 1. Where the time goes (from existing logs + one instrumented 2-node run)

dm at 6.441 × 10¹⁰ digits per node (S48 2 runs per size, O1 6-node run r12, this task's run `~/p1/i1/run.log`), seconds (m):

| nodes | dm | reciprocal (its products) | step 1: A·μ + X_hi·Q | step 2: A·μ | X_lo·Q | division products | shifts |
|---|---|---|---|---|---|---|---|
| 1 | 22.5 | 6.0 | — | — | — | — | — |
| 2 (S48 r1) | 61.3 | 18.4 (13.4) | 19.0 | 7.6 | 15.8 | 40.5 | 85 / 3.4 |
| 2 (P1 i1) | 55.4 | 13.6 (11.8) | 17.0 | 6.9 | 17.9 | 39.8 | 85 / 3.5 |
| 4 (S48 r1) | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 51.8 | 91 / 4.9 |
| 6 (O1 r12) | 117.3 | 25.9 (21.0) | 43.1 | 17.0 | 31.1 | 88.7 | 94 / 5.1 |

- The division's products are 72–76 % of dm at 2–6 nodes; **the two low products X·Q are the largest part**: at 2 nodes
  ≈ 10.8 s (X_hi·Q) + 11.7 s (X_lo·Q, without one 6.8 s exchange stall) of 39.8 s (m, per-piece trace `RNS_DIST_CACHE_TRACE=1`);
  at 6 nodes ≈ 26 + 31 s of 88.7 s (m, by difference).
- The grids (m, `ECALC_VERBOSE`): X·Q is s × 2s limbs mod B^(2s): **2 × 3 pieces of 2³² at 2 nodes (6 formed), 2 × 7 of 2³³ at
  6 nodes (13 formed)**; A·μ is 2 × 2 (3 formed) and 2 × 4 (7 formed). Each piece is one plane: forward of the inner operand,
  pointwise + inverse, plus the outer operand's forward when it is not cached.
- Per 2³²-point piece at 2 nodes (m, trace): 1.9–2.2 s with three transforms per prime, 1.5–1.7 s with the outer operand's
  transform cached; of that `ntt` 0.8–1.3 s, of which the all-to-alls are 0.77–1.14 s; redistribute 0.2–0.35 s, result out
  0.1–0.37 s, CRT 0.05 s. **The products are exchange-bound** (one all-to-all of the plane per transform per prime).
- The partial transform cache gets fewer primes in the second low product (X_hi is held by the writer): 1 of 3 primes at
  2 nodes, 2 of 3 at 6 nodes (m); that costs ≈ 1–5 s per division (mod).
- The grid shapes are already the best uniform shapes for these cuts (a skip-aware search over all ka × kb finds nothing
  better at 2, 4, 6 and 576 nodes, mod), so re-shaping gains nothing. Larger planes need larger pools (memory). The lever left
  is **fewer transforms for the same information**.

#### 2. The change

The low product only serves R = A' − X·Q, which is small (|R| < 65 Q < B^(w−1), w = n_Q + 2). With P = the group's plane cap
(2^k points) and h = w − P, M = B^h (B^P − 1) > 2|R|, and R is fixed by its residues mod B^h and mod B^P − 1 (coprime):

- L = X·Q mod B^h: the existing grid on the low h limbs of X and Q only (pieces at or above h skipped);
- U = X̃·Q″ mod (B^P − 1): **one cyclic plane of P points** (the pointwise product of two length-P transforms is the cyclic
  convolution); X̃ = X folded mod B^P − 1 (X itself when X ≤ P limbs), Q″ = Q·B^(−h) folded (once per division; B^(−h) mod
  B^P − 1 is a rotation: the fold's chunks start at h mod P);
- r_L = (A' − L) mod B^h, β = [A' mod B^h < L]; t = fold(A' >> h) + fold(L·B^(−h)) − β − U mod (B^P − 1);
- R_c = r_L + B^h·t ∈ [0, M) in basis w; R = R_c when R_c < B^(w−1), else R = R_c − M (X too large). The corrections then run
  unchanged (`mn_dkm_corr`).

Code: `newton_db.c` (`mn_wrap_plane`, `mn_wrap_a`, `mn_wrap_step`, `mn_fold`, `mn_modm1`, `mdb_zero`; `mdb_shift_gt` = `mdb_shift`
with a truncation length), `rns_dist.c` (`mn_core`'s cyclic mode `cyc` = P: the plane fixed at P, the value spans
[0, P + 4); `mn_grid`'s skip limit and result basis as parameters; `rns_mul_dist_mn_cyc_v`, `rns_mul_dist_mn_low_basis_v`,
`rns_mul_dist_mn_logmin`), `mdb.h`. Switch `DM_MN_WRAP` (0 off, the default; 1 the cap's plane when the low product would be a
grid; k ≥ 20 a plane of 2^k, tests). The one-node path is untouched. A Python model of the arithmetic (folds, CRT, both signs of
R, h > P, X > P) agreed with the exact R in 3000 random cases before the code was written.

Modelled transform volume of the two low products (units of one 2³³-point plane transform per prime, the outer operand
cached; mod): 2 nodes 7.0 → 4.0 (−43 %), 4 nodes 14 → 8, 6 nodes 28 → 19 (−32 %), 576 nodes 2304 → 1408 (−39 %). Cost added:
about 10 `mdb_shift`s per step (folds, the CRT assembly), ≈ 0.1–0.3 s each at 2 nodes (a).

#### 3. Correctness (m)

`tools/gate.sh --build` at 10⁹ digits, aac7, queue item `72_p1_gates2` (`~/p1/g2/summary.txt`), 2026-10-12 15:22–15:31:

| gate | nodes | env | result | wrap used |
|---|---|---|---|---|
| off2 | 2 | — | PASS: VERIFY OK, digits identical | — |
| w24 | 2 | `DM_MN_WRAP=24` | PASS | yes (h > P: multi-chunk folds) |
| w21 | 2 | `DM_MN_WRAP=21` | PASS | yes |
| w22gen | 2 | `DM_MN_WRAP=22 DIST_GEN=1` (the general map of non-power-of-two groups) | PASS | yes |
| w24hi3 | 2 | `DM_MN_WRAP=24 NEWTON_DKM_TEST_HI=3` (X_hi − 3: up-corrections) | PASS | yes |
| w24him3 | 2 | `DM_MN_WRAP=24 NEWTON_DKM_TEST_HI=-3` (X_hi + 3: R < 0, the M − R_c branch) | PASS | yes |
| w24tc | 2 | `DM_MN_WRAP=24 ECALC_TEST_CORR=2` | PASS | yes |
| w1 | 2 | `DM_MN_WRAP=1` | PASS | no (the low product is one plane at 10⁹) |
| off1 / w1n1 / w24n1 | 1 | — / `=1` / `=24` | PASS | no (size 1) |

After the rebase on main f08a4ea (item `77_p1_gates3`, `~/p1/g3/`, 2026-10-12 19:30–19:44): off2, w24, w24him3, w1 at 2 nodes and off1,
w24n1 at 1 node all PASS (VERIFY OK, digits identical); main's gate now forces P24 at 2 nodes (pool 0: 4 primes), so the wrap ran
with P24 on (the low part in 24-digit points, the cyclic plane in 18-digit points at four primes).

#### 4. Timing (paired ABBA, 2 nodes)

Method: TESTING.md §6. 2 nodes, 2 × 6.441 × 10¹⁰ = 1.2882 × 10¹¹ digits, no digit output, the aac7 launch line (the `LINE` of
`ecalc/e16_headline.sh`) + `COMM_XSTATS=1 MN_WAIT_STATS=1`; A = default, B = `DM_MN_WRAP=1`; one binary (branch `p1` 13aa201,
SHMEM build); rounds A B B A in one 2-node hold each (queue items `75_p1_abba1b`, `76_p1_abba2`, driver `~/p1/p1_abba.sh`,
2026-10-12 15:33–16:16). The stop rule (p < 0.05) fired after round 3. All 12 runs VERIFY OK on both nodes.

| quantity (s) | A mean (6 runs) | B mean (6 runs) | B − A per round | B − A mean, 95 % CI (3 pairs of rounds) |
|---|---|---|---|---|
| `total` | 140.4 | 129.4 | −13.6, −7.4, −12.1 | **−11.1 [−19.1, −3.1], p 0.027** (m) |
| dm | 54.5 | 46.8 | −8.8, −6.7, −7.5 | −7.7 [−10.3, −5.1] (m, same rounds) |
| division products (X·Q, A·μ) | 38.5 | 28.1 | | −10.4 (m, means) |
| shifts (count / s) | 85 / 3.45 | 97 / 5.90 | | +2.45 (m, means) |
| step 2's X_lo·Q + corrections | 15.5 | 11.3 | | −4.2 (m, means) |

`total` moves more than dm because bs varies between runs (43–50 s, both arms); dm is the phase the change touches. Memory (m,
`ECALC_LIVE` lines): at the step-2 low product the block pool is **57.98 GB per APU under B against 53.69 under A** (+4.3 GB per
APU, +17 GB per node: the pool grew for the wrap's numbers), live 25.8 vs 28.6 GB, VmHWM unchanged (28.9 GB).

**At 6 nodes (mod, not measured):** the two low products ≈ 57 s (m, O1 r12); the transform units fall 28 → 19 (−32 %); scaled by
the 2-node ratio of measured to modelled saving (−37 % m vs −43 % mod on the X·Q part) ≈ −15.6 s of products, less ≈ 21 extra
shifts at 0.25–0.4 s (a) = +5..+8 s: **≈ −8..−11 s of dm at 6 nodes** (mod). 576 nodes: −39 % of the X·Q transform volume (mod;
seconds not modelled — `estimate.py` has no term for it).

#### Verdict

Needs the user's decision; recommended for a 6-node A/B next. At 2 nodes it saves **11.1 s of `total` (95 % CI 3.1–19.1 s,
3 rounds, m)**, 7.7 s of dm, digits identical in every gate. Costs: +4.3 GB per APU of block pool at the division at 2 nodes (m);
not in the memory model; not timed at 6 nodes or with P24 (the target's mode; gated with P24). Switch stays off by default.

#### Open issues

- Not run at 6 nodes (the brief allows ≤ 2 nodes); the general map is covered by `DIST_GEN=1` at 2 nodes only.
- P24 (the target's mode when pool 0 holds four planes): the cyclic plane runs in 18-digit points (B^P − 1 needs whole decimal
  limbs); gated at 10⁹ with P24 forced (§3), not timed with P24.
- The memory model (`dm_layout`, `mem_model.py`) does not count the wrap's numbers (Q″ and the fold accumulators, basis
  P + 4); measured +4.3 GB per APU of pool growth at 2 nodes. At the target's margin this must be modelled before use.

#### RESUME

Done: merged to main (switch off), worktrees removed. Raw outputs on aac7 `~/p1/` (i1: the instrumented run; g2: gates;
abba: `totals.tsv`, `stats.txt`, logs; driver `~/p1/p1_abba.sh <rounds>`). Next (user's call): a 6-node ABBA of `DM_MN_WRAP=1`
(≤ 4 rounds, ~6 min per run), the wrap's numbers in `dm_layout` / `mem_model.py` before any
target use.

## RECIP — the Newton reciprocal's error: the t_newton bound or the reciprocal?

(former file: RECIP.md)

**Question.** `t_newton` (section 5, in SIMP's run) found one divisor Q whose reciprocal μ ≈ B^(n_Q+k)/Q was −22 units off
GMP's floor at n_Q 131075, k 65538, beyond the test's ±8 bound (results on branch simp, `results/CODE_REDUCTION.md §SIMP`, "Doubt"). Is the
bound wrong or the reciprocal? Can the error make a division wrong?

**Change / branch.** Branch `recip`. Switch `NEWTON_CHAIN_GUARD` (default 0, off): the reciprocal's anchored chain keeps one
guard limb per doubling. `t_newton`: the bound of sections 2 and 5 is the derived one (`recip_bound`), new section 8.

**Answer.** Both, in different ways.

1. **The −22 is the bound's fault (the reciprocal is as designed).** The test's ±8 units assume a normalised Q. With
   decimal limbs Q is not normalised, and one unit of μ is relative to M = B^n_Q/Q, which ranges from 1 to B. SIMP's Q has
   top limb 2.349·10¹⁶ (M = 42.6); its chain's last doubling 32769 → 65538 is exact (jn = 2j, no limb cut after it), and such
   a doubling keeps its own rounding error, at most 2M + 3 units. −22 = −0.52·M. A host GMP model of `recip_db2` with exact
   products reproduces it bit for bit (−22), and reproduces SIMP's three measured errors on the rng5 operands (+1, +0, +1).
2. **But the reciprocal has a real weakness the test never reached.** When the chain has several exact doublings in a row
   (any k with ⌈k/2^i⌉ even for several consecutive i, e.g. k = 2^m), each one squares the error left by the previous one
   (in units of the target precision, ε → −ε² + rounding). Modelled: k = 512 … 2048 at n_Q 1027 give errors up to 10¹²–10¹⁹
   units (one limb lost) for some Q with M ≈ 1, and up to 10³⁵ units (two limbs) at h = 2048. The division's corrections
   absorb errors up to ~64·B units of μ where μ's top limbs are used uncut; past that, DKM's step 1 dies with
   `DKM step 1: N corrections` (fatal; the corrections never produce wrong digits). Modelled: for a quotient of 4095 limbs
   (h = k₁ = 2048) 3 of 900 step-1 estimates are off by more than 64 (up to 1.8·10⁸).
3. **The fix is one guard limb per level** (`NEWTON_CHAIN_GUARD=1`): targets k, ⌈k/2⌉ + 1, ⌈(⌈k/2⌉ + 1)/2⌉ + 1, … so every
   doubling j → jn has jn ≤ 2j − 1 and its output is cut by ≥ 1 limb. Modelled: error 0 units in every sweep below, with
   exact or worst-case cut products; the DKM estimates exact. Cost: about two more limbs per level (≤ 2 extra tiny doublings
   at the bottom; at most 19 doublings instead of 17 up to k = 2·10⁵), negligible (modelled). Digits cannot change: the
   division corrects to the exact quotient either way.

#### Derivation (one doubling, correction form, ALGORITHM.md §5.1)

r ≈ y_j = B^(n_Q+j)/Q at precision j, relative error e (r = y_j (1 + e)); M = B^n_Q/Q; one unit of r is 1/(M B^j) relative.

- t = Q_t·r, Q_t = Q's top `take` limbs (low by α < 1 unit of Q_t; α = 0 when take = n_Q); u = ⌊t/B^(take−j)⌋ − c₁ (c₁ ≤ 1:
  the low cut); d = B^(2j) − u = D + γ with D the exact value and γ = α r/B^(take−j) + β + c₁ ∈ [0, 2 + 1/B) (β the floor).
- corr = ⌊r|d|/B^j⌋ − c₂ (c₂ ≤ 1), r' = r B^j ± corr. Exact Newton gives Y(1 − e²), Y = B^(n_Q+2j)/Q. So
  **r' − Y = −Y e² + (r/B^j) γ ± (β₂ + c₂)**, with r/B^j ≈ M and Y e² = δ²/M for an input error of δ units.
- If the target jn < 2j the result is cut by 2j − jn ≥ 1 limbs: the M-scaled term (≤ 2M + 3 ≤ 2B + 3) becomes ≤ 3 units.
  If jn = 2j (the target even) nothing cuts it: **err = ⌊Y⌋ − μ ≥ −(2⌈M⌉ + 4)** after a single exact doubling, and the next
  doubling's quadratic term turns δ ≈ 2M into (2M)²/M = 4M; in normalised units ε = δ/M a run of exact doublings maps
  ε → −ε² + γ, which diverges once |ε| ≥ 2 (M ≈ 1, where the ±1 roundings are ±1 normalised). The step's convergence check
  (corr < B^(j+1)) only catches |ε| M ≥ B, so the last doubling's output can carry up to ~B²/M units.
- The division (DKM, ALGORITHM.md §5.2) uses μ's top k₁ + 1 limbs; X₀'s error is ≈ Δ/B for a μ error of Δ units when h = k₁
  (odd quotient length with the reciprocal at exactly h: the sharded flow when S has one limb more than P; size 1's prewarm
  asks k + 1 and gets h + 1, which divides Δ by B again). The correction loops stop at 64 (fatal).
- Production exposure (modelled): the run of exact doublings at the top of the chain has length ≥ 8 for ~0.4 % of k (random k
  in 10⁷ … 10¹⁰); runs of 5–7 at the top give μ errors of 10³–10⁷ units, harmless (X₀ exact). The fatal needs a long run,
  h = k₁ and an unlucky Q (1–4 % of Q at k = 2^m): roughly 10⁻⁵ per run size (modelled, order of magnitude). For 10⁹ … 10¹² digits
  (k estimated from log10 N!/18, assumed) h is odd: a top run of 0.

#### Method

- Local model (no GPU): a C/GMP program reimplementing `recip_db2` step by step (seed `newton_seed_top`, take, middle/whole
  product as exact products, the low cuts as 0 / always −1 / random −1 per product, the convergence check, the overshoot
  shrink, the anchored chain), and DKM's step-1 estimate (`dkm_est_db`). It replays `t_newton 20`'s random stream of SIMP
  4003968 up to section 5 (and the rng5 stream), and sweeps uniform Q. Source kept in the local archive
  (`archive:drivers/recip/rmodel.c`).
- The SIMP operand: section 5's rng state before the draw is `0x9A4EA83C95610E7B` (top limb 23490058283511592, measured by
  the replay); `t_newton` section 8 (a) draws it directly.
- GPU (aac7, 1 node, one queue item): the build, `t_newton 20` with the switch off and on, and the 1-node gate off and on.

#### Results

| check | M | k | error, guard off | error, guard on | label |
|---|---|---|---|---|---|
| SIMP's Q, n_Q 131075 (model replay) | 42.6 | 65538 | **−22** (= SIMP's GPU value) | 0 | modelled |
| rng5 Q, n_Q 131075, k = 65538 / 131075 / 262150 (model) | 1.16 | | +1 / 0 / +1 (= SIMP's GPU values) | | modelled |
| sweep n_Q 1027, k 514 (one exact doubling), 300 uniform Q | 1 … | 514 | −174 … +1 (exact products); −479 … +2 (cuts); max \|err\|/M 2.47 | 0 | modelled |
| same, top limb uniform below B/10 (large M) | up to 10¹⁷ | 514 | −1279 … 0; max \|err\|/M 1.04 (exact), 1.96 (cuts) | 0 | modelled |
| k = 512 (9 exact doublings in a row), 150 Q | | 512 | up to 3.2·10¹² (exact), 7.3·10¹⁸ (cuts) | 0 | modelled |
| k = 1024 / 2048 | | | up to 4.5·10¹³ / 1.1·10¹² (exact), ~10¹⁸ (cuts) | 0 | modelled |
| k = 513, 1027 (odd: the last doubling cut) | | | 0 | 0 | modelled |
| derived bound −(2⌈M⌉ + 6) … 8, chains with no two exact doublings in a row | | | 0 violations in 2 × 3 × 300 Q | | modelled |
| DKM step 1, n_Q 8200, quotient 4095 (h = k₁ = 2048), 300 Q × 3 A | | 2048 | \|X₁ − X₀\| up to 1.8·10⁸; 3 of 900 beyond 64 (fatal) | 0; 0 of 900 | modelled |
| same, quotient 2047 (h 1024) / 8191 (h 4096) | | | up to 3.2·10⁴ / 7.3·10¹⁵; 3 / 9 of 900 beyond 64 | 0 | modelled |
| same, quotient 3065 (h 1533, odd: top run 0, a run of 7 inside) | | | 0 | | modelled |
| same, top runs 1 … 7 (h 1522 … 1664) | | | μ error 84 … 4.5·10⁷ units; X₀ within 1 | | modelled |

GPU (aac7, 1 node, queue item `50_recip_gpu`, job 12775, commit e475e29; logs in `~/recip/` on aac7):

| check | guard off | guard on | label |
|---|---|---|---|
| `t_newton 20` (sections 1–8, 4 at 2^20) | **VERIFY OK (2206 checks)**, the new bound in sections 2 and 5 | sections 1–5 passed, then the step hit the item's 300 s timeout in section 6 (not a failure; sections 6–7 with the guard on not run) | measured |
| after the rebase onto SIMP + ADOPT (24446df, item `51_recip_rebased`): gate 1 node; `t_newton 20` | gate **PASS** (VERIFY OK, digits identical, ecalc total 13.29 s); **VERIFY OK (2756 checks)**, section 8 identical to the above | `t_newton 20` with `NEWTON_CHAIN_GUARD=1`: **VERIFY OK (2756 checks)**, all sections | measured |
| 8 (a) SIMP's Q, n_Q 131075, k 65538 (top limb 23490058283511592) | **−22 units** (bound [−92, 8]) | **0** | measured |
| 8 (b) n_Q 1027, k 1024, 100 uniform Q | **15** beyond ±8, largest \|error\| **9.9·10²⁴** units | 0 beyond ±8, largest 0 | measured |
| 8 (b) n_Q 1027, k 2048, 100 uniform Q | **9** beyond ±8, largest **2.0·10²⁷** units | 0, largest 0 | measured |
| 8 (c) DKM step 1, n_Q 8200, quotient 4095 (h = k₁ = 2048), 300 Q × 3 A, device μ | max \|X₁ − X₀\| **1.83·10⁸**, **3 of 900 beyond 64** (= the model's numbers exactly) | **0**; the 6 divisions run (3 first Q + the 3 fatal-size cases) exact against GMP, step-1 corrections ≤ 4 | measured |
| gate, 1 node, 10⁹ digits | **PASS**: VERIFY OK, digits identical, ecalc total 14.45 s | **PASS**: VERIFY OK, digits identical, ecalc total 11.82 s (one run each: not a timing comparison) | measured |

The device reciprocal behaves as the model says: the same −22 on SIMP's Q and the same DKM estimate errors (1.83·10⁸, 3
of 900) on the device μ; the larger errors at k = 2^m on the GPU (10²⁴ vs the model's 10¹³ with exact products) come
from the low cuts (the model's cut modes give up to ~10¹⁹ at n_Q 1027). The guard removes all of it.

#### Verdict

(a) The −22 is a bound problem: the bound now scales with M (`recip_bound`: −(2⌈M⌉ + 6) … 8 when the last doubling is
exact, ±8 otherwise, no bound with two exact doublings in a row). (b) The reciprocal does have a real, rare failure mode
(fatal, never wrong digits) at chains with long runs of exact doublings; `NEWTON_CHAIN_GUARD=1` removes it at no measurable
cost (modelled; the 1-node gate with it passes, measured). Adoption is the user's: it changes no digits, and only the reciprocal's last few limbs and step counts.

#### Open issues

- The 2-node gate with the switch on is not run (the item budget was 1 node); the sharded chain (`recip_mn_body`,
  `newton_mn_chain_start`) follows the switch. Needed before a default flip.
- The gated commit is 24446df (on SIMP + ADOPT); the merge is rebased once more onto SH3 + K4c (dfac293), whose changes
  (rns_dist.c, rns_mul.c, p24_crt.c, tests) do not touch this branch's files except the generated switch tables; not re-gated.
- `mem_model.py` does not mirror the switch's +1 limb in `dm_layout`'s jl (off by default; within the hole's slack).
- The fatal case was not provoked on the GPU through the division (it would abort the test); section 8 (c) computes the
  off-path step-1 estimates from the device μ instead (3 of 900 beyond the 64 limit, measured).
- The `newton_mn_chain_start` rewrite is identical with the switch off (all k < 2·10⁵ and 2³³ compared against the old
  loop, measured on the host); the off path's 2-node gate was not rerun.

#### RESUME

Merged to main (branch `recip` deleted). Done: analysis, model, switch (off), test, docs, 1-node gates off/on, t_newton on
the GPU. Next, if the user wants the guard as the default: the 2-node gate with `--env NEWTON_CHAIN_GUARD=1`, `t_newton 20`
under the switch is done (VERIFY OK), `mnaccept.sh --only unit,e9,mn`, and a 1-node 10¹⁰ ABBA to show no cost.

## R415 — R4 (Phase 15, row 1b): the reciprocal's Q_t·r as a middle product (V3 item 4 = PLAN §33 B2)

(former file: R415.md)

Branch `p15-R4` from int15 @ 0b80ca3 (B0). aac6 clone `~/ntt-R415`. Times Eastern (aac6 prints Central; +1 h). Every number
is labelled **measured** / **modelled** / **assumed**.

Files: `ecalc/newton_db.c` (the switch, `recip_db2`, `recip_mn`), `tests/t_newton.c` (section 5), `ecalc/README.md` (one row).
Minimal helpers elsewhere (named): `ecalc/rns_dist.c` + `ecalc/rns_mul.h` — `rns_mul_band_db` (one line: `mul_grid` with both
cuts, truncated to w limbs); `ecalc/newton.h` — `struct newton_mid_stats`, `newton_mid_st`, `newton_recip_set` (a test hook);
`ecalc/mn_plan.h` / `mn_plan.c` — `newton_recip_mid` and the plan printer following the switch (as R1 did for the cut).

#### 1. The exactness argument (written before the node runs)

##### 1.1 What the step reads

One round of `recip_db2` (and `recip_mn`, the same over shares) at precision j: take = min(2j + 2, n_Q), Q_t = the top take
limbs of Q, t1 = Q_t · r, v = take − j, **u = ⌊t1 / B^v⌋**, e = u − B^{2j}. The round reads u **only through (neg, d)**:
neg = [e > 0], d = |e| (then corr = ⌊r d / B^j⌋ >> …, r' = r B^j ∓ corr, the converged / overshoot tests — all functions of
r, neg, d). Under `NEWTON_RECIP_CUT` (the default) t1 is replaced by t1 − S, S the skipped low pieces (< B^{v−1}), so the round
reads u_cut = ⌊(t1 − S)/B^v⌋ (R114 §1.2). Everything below holds for S = 0 (cut off) too.

##### 1.2 What the band product computes, and why the dropped pieces cannot reach the band (carries included)

`rns_mul_band_db(C, A, B, c, w)` is `mul_grid(C, A, B, c, w)` truncated to w limbs; w = take + 4 (`MID_HI` = 4). `mul_grid` skips
a piece (i, l) when oa + ob ≥ w (the high cut) or oa + ob + la + lb ≤ c (the low cut, `grid_piece_skipped`), forms every other
piece exactly and adds it **whole** (a full-length shifted add). A high-skipped piece is a_i b_l B^{oa+ob} with oa + ob ≥ w: a
**multiple of B^w**. So, as integers,

  C_full = t1 − S − H,  H ≡ 0 (mod B^w),  hence  C := C_full mod B^w = (t1 − S) mod B^w.   (1)

Carries: reduction mod B^w is a ring homomorphism, so (1) holds whatever the carries inside the formed pieces and their sum do;
a carry only moves upward, and every carry that H would have produced starts at a limb ≥ oa + ob ≥ w, above everything kept.
The carries of the formed pieces into limbs ≥ w are discarded by the truncation, which changes no limb below w. A product that
fits one plane is formed whole and then truncated: (1) trivially. Over shares (`rns_mul_dist_mn_cut(…, lowcut, highcut = w)`,
`mn_grid`): the same skip rule, the piece windows clipped to basis w and the carry out of the top dropped — the reduction mod
B^w the division's low product X Q mod B^w relies on in every multi-node run.

Write t1 − S = C + B^w M (M ≥ 0). Since B^v | B^w and w − v = j + 4:

  ⌊C / B^v⌋ = ⌊(t1 − S)/B^v⌋ − B^{j+4} M,  i.e.  **U := ⌊C / B^v⌋ = u_cut mod B^{j+4}**.   (2)

So the band product delivers t1's limbs [v, v + j + 4) = [take − j, take + 4) exactly as the cut path has them.

##### 1.3 The bound that makes the band enough

For j ≥ 4, B^{2j} ≡ 0 (mod B^{j+4}), so U ≡ e (mod B^{j+4}). If **|e| < B^{j+3}**, e is fixed by U:
e ≥ 0 ⇔ U < B^{j+3} (then e = U), e < 0 ⇔ U > B^{j+4} − B^{j+3} (then e = U − B^{j+4}); the two ranges are disjoint.
The code (`mid_d`, and its twin in `recip_mn`) takes the first when U has ≤ j + 3 limbs, the second otherwise, and **stops the
run (fatal) if the resulting d has more than j + 3 limbs** — the bound is proven below; the check turns any violation into an
abort, never into a silent wrong u.

**Claim.** If the previous round of the chain (at precision j_p, take_p, ρ_p = B^{take_p}/Q_{t,p} ∈ (1, B]) did not overshoot and
had **d_p = |e_p| < B^{j_p+1}** (`mid_ok`), then the current round has |e| < 5 B^{j+2} + 2 < B^{j+3}.

Proof (R114 §1.1's algebra; E = R_j − r the iterate's error in units of r's last limb, R_j = B^j ρ):
1. The previous round's own identity: e_p = −E_p B^{j_p}/ρ_p − θ_p − δ₁ (θ_p ∈ [0, 1) the floor of u, δ₁ ∈ {0, 1} the cut), so
   |E_p| ≤ (d_p + 2) ρ_p / B^{j_p} < (B^{j_p+1} + 2) ρ_p / B^{j_p}. In particular r_p = R_{j_p} − E_p < B^{j_p+1} + B² + 2B < B^{j_p+2}.
2. Its output r' = r_p B^{j_p} ∓ corr has error E' = E_p²/ρ_p − r_p(θ_p + δ₁)/B^{j_p} ± φ ± δ₂ (φ ∈ [0, 1) corr's floor, δ₂ ∈ {0, 1}
   the cut of r d), so |E'| ≤ E_p²/ρ_p + 2 B² + 2.
3. Every path to the current round's r is r' shifted right by s ≥ 0 limbs with a floor (converged: s = 2j_p − j ∈ {0, 1} at the
   anchored targets; repeat: s = j_p, same j): |E''| ≤ |E'| / B^s + 1 ≤ |E'| + 1.
4. The current round's target uses take ≥ take_p: Q_t = Q_{t,p} B^Δ + (lower limbs), so 0 ≤ ρ_p − ρ ≤ ρ_p / Q_{t,p} ≤ B^{2 − take_p}
   and B^j (ρ_p − ρ) ≤ B^{j + 2 − take_p} ≤ 1 (take_p = 2j_p + 2 ≥ j + 2; or take_p = take = n_Q and the difference is 0):
   |E| ≤ |E''| + 1.
5. The current round: e = −E B^j/ρ − θ − δ₁, so |e| ≤ |E| B^j / ρ + 2, and with ρ > 1, ρ_p/ρ ≤ 1 + B^{1 − take_p} ≤ 2:
   |E|/ρ ≤ (d_p + 2)² (ρ_p/ρ) / B^{2 j_p} + (2 B² + 4)/ρ < 2 B² (1 + 2 B^{−j_p−1})² + 2 B² + 4 < 4 B² + 5,
   so |e| < (4 B² + 5) B^j + 2 < 5 B^{j+2} < B^{j+3} for B ≥ 6 (B = 2⁶⁴ or 10¹⁸). ∎

So one guard limb above j + 2 would do (|e| < B^{j+2}·5 needs U mod B^{j+3} and 5 B^{j+2} < B^{j+3}/2); `MID_HI` = 4 keeps two, as
V3 sized it (w = v + j + 4), for margin and a check that is a limb-count test.

##### 1.4 Consequence: the same bits

Under the claim, (neg, d) from U equal (neg, d) from u_cut, so r', corr, the converged and overshoot decisions, every later
round, **μ are bit-identical to the switch off** (with the cut on or off). No new error term, no change in the correction
counts, nothing for the division to absorb. (This is stronger than R114's cut, which adds up to one unit per round.)

##### 1.5 When the whole product is formed (`mid_ok` = 0)

- The first round after the seed (the seed's error is not bounded by a previous round).
- The round after an **overshoot** (r is shrunk by 1/16, not the output of a round).
- The round after one with d ≥ B^{j_p+1} (a far iterate: the perturbed-seed / low-seed tests, the early rounds).
- `recip_mn`'s first sharded round (r0 comes from the single-node chain; its last d is not carried over — conservative; that
  product is small, j ≈ 2¹⁶), and **the first round after the step's group changes** (X1 subgroups at sizes > 2: a joining node
  has no previous round, and all members must form the same product — added in a828807 after batch B; d.n is a group-wide value,
  so within one group every node takes the same decision).
- j < 4 or j > take (nothing to skip / the rare shifted form).
The bookkeeping is `mid_ok = (d.n <= j + 1)` after every round's d, and `mid_ok = 0` on the overshoot path.

##### 1.6 What is saved (plan printer on the login node, **measured** decisions of the code on predicted sizes)

`NEWTON_RECIP_MID=0/1 MN_PLAN_ONLY=<d>:<g> ./ecalc`:

| digits : nodes | top doubling's Q_t·r grid | formed, cut only → cut + middle | recip pieces |
|---|---|---|---|
| 10¹¹ : 1 | 9 × 3 (3·2²⁹ pieces, B form) | 24 → **20** | 97 → 93 |
| 1.3 × 10¹¹ : 1 | 6 × 4 (2³¹, C form) | 21 → **18** | 103 → 100 |
| 4.25 × 10¹³ : 576 | 5 × 2 (2⁴⁰) | 9 → **8** | 67 → 66 |

These match V3's model (`tests/v3_b2.py`: 24 → 20 at 10¹¹, 9 → 8 at 576). Not built: V3's "wrapped one-plane" form (a cyclic
convolution of ≥ 2j + 3 limbs for products that fit one plane; −0.5 s modelled, ½ d).

#### 2. Tests (all **measured**; aac6, logs in `~/R415/<batch>/`)

Batch A — job 21463, s24-16, 2026-09-26 23:08–23:39 EDT, commit 57c897e (`~/R415/A.steps`, run in `~/ntt-R415/ecalc`):

| test | command (on the node) | result | wall |
|---|---|---|---|
| t_newton binary, cap 2²⁰ | `NEWTON_DEVICE=1 NEWTON_RECIP_MID=1 DIST_LOGN_TEST=20 tests/t_newton 20` | VERIFY OK (906 checks) | 67 s |
| t_newton decimal, cap 2²⁰ | `LIMB_BASE=10` + the same | VERIFY OK (906) | 270 s |
| t_newton decimal, cap 2²⁰, cut off | `… NEWTON_RECIP_CUT=0 …` | VERIFY OK (906) | 271 s |
| t_newton decimal, real cap | `LIMB_BASE=10 NEWTON_DEVICE=1 NEWTON_RECIP_MID=1 tests/t_newton 20` | VERIFY OK (906) | 269 s |
| t_newton binary, switch off | `NEWTON_DEVICE=1 tests/t_newton 20` | VERIFY OK (906) | 65 s |
| t_mul | `tests/t_mul 20` | VERIFY OK (189) | 293 s |
| e9 decimal / binary | `NEWTON_RECIP_MID=1 [LIMB_BASE=2] ./ecalc 1000000000 f` | **identical** to ref/e_1000000000.txt (cmp); total 10.5 / 20.9 s | |
| e9 decimal / binary, cap 2²⁴ | `… DIST_LOGN_TEST=24 …` | **identical**; 12.4 / 22.3 s | |
| 10¹⁰ | `NEWTON_RECIP_MID=1 ./ecalc 10000000000 f` | **identical** to results/e_1e10.out (cmp); total 24.0 s | |
| 10¹⁰, cap 2²⁸ | `… DIST_LOGN_TEST=28 …` | **identical**; 30.3 s | |
| 10¹¹ | `NEWTON_RECIP_MID=1 ./ecalc 100000000000 f` | sha1 **578f5efb…** = `~/V214/e_1e11.sha1` (**identical**); total 238.4 s; top doubling 8 skipped / 27 formed (4 / 31 off) | 297 s |

Section 5 of t_newton (new): the band product equals the whole product mod B^w and is within one unit on [v, w) with the low cut
(0 in every case measured); the reciprocal with the switch is **bit-identical** to the one without, cut 0 and 1, nq 4097 … 2²⁰ + 1,
k = nq/2 + 1, nq, 2 nq, uniform / all-ones / single-bit, error vs GMP −1 … +0 units; 9–18 rounds per reciprocal on the middle
product. **Weakness found**: at cap 2²⁰ these sizes give k × 1 grids (r in every piece), so the high cut skipped nothing in the
test — the skip was exercised only by the 10¹¹ run (and e9/e10 at lowered caps, where the grids are also k × 1). Fixed in ea186c7:
band sizes 3·2²⁰ × 1.5·2²⁰ and 2²² × 2²¹, a reciprocal at 2²² + 1 limbs whose top grid splits both operands, and a check that the
switch skips more pieces than without (batch E below).

Batch B — job 21475, s24-30, 2026-09-27 01:33–01:36 EDT: `NEWTON_RECIP_MID=1 ./mnaccept.sh $J --only mn`: **2 passed, 3 failed**
(e8 sizes 3, 4 and e9 size 4: `comm_tcp: alltoallv count mismatch` in the reciprocal). Sizes 2 passed, also at cap 2²⁰ / 2²²
(identical). **Cause**: at sizes > 2 the X1 rule runs the early sharded steps on a subgroup; `mid_ok` was per node, and a node
joining the group had no previous round (mid_ok 0) while the old members had 1 — the members formed different products. **Fix**
(a828807): `mid_ok = 0` whenever the step's group changes (all members agree; the plan printer follows: at 576, 67 → 66 pieces
unchanged). Rerun in batch E.

Batch E — job 21517, s24-26, 2026-09-27 10:57–11:08 EDT, commit a828807 (`E.steps`):

| test | result |
|---|---|
| `NEWTON_RECIP_MID=1 ./mnaccept.sh $J --only mn` | **5 passed, 0 failed** (e8 sizes 2, 3, 4; e9 sizes 2, 4; all identical) |
| mn e8 sizes 2, 3, 4 at cap 2²⁰ (`POOL_LOG=27 DIST_LOGN_TEST=20`) | all identical, all nodes VERIFY OK; the top Q_t·r over shares a 5 × 3 grid, 2 pieces skipped by the high cut (sizes 2, 3) |
| mn e9 sizes 3, 4 at cap 2²² (`POOL_LOG=29`) | identical; e.g. size 3: 15 × 6 grid, 33 skipped with both cuts |
| e9 decimal / binary (switch on) | identical (the one-node path after the fix) |
| t_newton binary / decimal, cap 2²⁰, switch on (new section 5) | VERIFY OK (814 checks) each, 74 / 321 s; band 3·2²⁰ × 1.5·2²⁰: 1 of 18 pieces skipped, 2²² × 2²¹: 5 of 35, mod B^w identical; reciprocal 2²² + 1 limbs: pieces skipped 0 → 6 (cut off), 13 → 19 (cut on), 3·2²⁰: 0 → 2, 5 → 7; **μ bit-identical** in every case, error +0 |

Not rerun after a828807 (the fix touches only `recip_mn`, never called at size 1): t_mul, 10¹⁰, 10¹¹ (batch A / C / D on
57c897e cover the one-node path).

#### 3. Timing at 10¹¹ (all **measured**; same commit 57c897e; the change after it touches only `recip_mn`, not the one-node path)

C: with the digit file, job 21488, s24-16, 2026-09-27 01:44–02:10 EDT, order off/on/off/on. D: without the digit file, job 21492,
s24-26, 02:24–02:46 EDT, order on/off ×3. Wall = the `srun` of the process (start to exit, digit write joined), from the driver.
Every run: corrections 0/2, and with the file sha1 578f5efb… (identical).

| | recip(db) | recip line | dm | total (with file / without) | wall |
|---|---|---|---|---|---|
| C off 1, 2 | 40.42, 40.12 | 41.87, 41.49 | 100.30, 99.94 | 240.81, 237.32 | 299.8, 296.1 |
| C on 1, 2 | 36.66, 37.20 | 38.09, 38.57 | 96.46, 97.64 | 237.48, 229.68 | 296.0, 287.7 |
| D off 1, 2, 3 | 39.86, 40.19, 40.01 | 41.22, 41.57, 41.39 | 98.54, 99.58, 99.16 | 210.01, 210.94, 210.75 | 217.9, 218.8, 220.0 |
| D on 1, 2, 3 | 37.03, 36.21, 36.55 | 38.41, 37.63, 37.93 | 96.50, 95.12, 95.77 | 207.20, 206.01, 206.59 | 214.4, 213.2, 214.2 |

Means, on − off: **reciprocal 41.51 → 38.13 s (−3.38 s, −8.1 %)** over all ten runs — V3 modelled −3.4 s (−8 %); **dm 99.50 →
96.30 s (−3.2 s)**. Without the digit file: **total 210.6 → 206.6 s (−4.0 s)**, **wall 218.9 → 213.9 s (−5.0 s)**. With the digit
file: total 239.1 → 233.6 s, wall 298.0 → 291.9 s (−6 s, two runs each; this pair is noisier — bs itself moved 102 → 96 s in the
fourth run — the change affects only dm, so −3.2…−4 s is the reliable figure). 576 target: 9 → 8 pieces in the top doubling's
Q_t·r, −1.0 s (V3, **modelled**; not measurable here).

#### 4. Open issues

- The wrapped one-plane form (V3: −0.5 s more at 10¹¹, modelled) is not built.
- The digit-file pair (C) is noisy (bs varied 96–102 s); the reliable measured gain is the reciprocal's −3.4 s and dm's −3.2 s;
  the no-file wall −5.0 s (three pairs). A five-run paired series would tighten the with-file wall if the user wants it.
- `recip_mn` restarts the middle product after every group change (conservative; the X1 subgroups occur only in the small early
  steps). The first sharded round could inherit the single-node chain's last d; worth nothing measurable.
- 576 target: 4.25 × 10¹³ digits in ≈ 4.1 min (PLAN §36.10, **modelled**, part file at an **assumed** 2 GB/s); this switch takes
  ≈ 1.0 s (**modelled**, V3) off it: the top doubling's Q_t·r forms 8 of 10 pieces instead of 9 (plan printer).
- Nothing outside the named files. The switch stays off by default; adoption is the user's decision.

#### RESUME

**Done.** Branch `p15-R4` (commits 57c897e … this one). Every planned test ran: A (job 21463), B (21475: the group-change bug),
C (21488), D (21492), E (21517: the fix, mnaccept mn 5/5). No job of mine is queued or running. Scripts in `archive:results_raw_2026-10-19/R415/`,
logs in `~/R415/` on aac6; the clone `~/ntt-R415` is at a828807 (docs-only commits after it).

## R114 — R1 (Phase 14, W1): E7 the reciprocal's products cut to the band read; E9 Karatsuba over the grid (model); E10 the 576 tree-level spill (model)

(former file: R114.md)

Branch `p14-R1` from main @ ae1d872. Files: `ecalc/newton_db.c` (the reciprocal's product calls), `ecalc/rns_dist.c` /
`rns_mul.h` (a skipped/formed counter), `ecalc/mn_plan.c` (the plan printer follows the switch), `ecalc/mn_model.py` (E7 / E9 /
E10 terms), `ecalc/README.md` (switch rows). Every number below is labelled **measured** / **modelled** / **assumed**.

#### 1. E7 — the exactness argument (written before the code)

##### 1.1 What the reciprocal computes (newton_db.c `recip_db2`, lines 68–113; `recip_mn` 590–700 is the same over shares)

One step at precision j (r has j + 1 limbs, B = 2⁶⁴ or 10¹⁸):

| line | operation | what is read |
|---|---|---|
| 81–82 | t1 = Q_t · r (Q_t = the top `take` = min(2j + 2, n_Q) limbs of Q) | **u = t1 >> (take − j)** (85; the `else` at 86, j > take, never occurs for k ≤ n_Q and gets no cut) |
| 87–88 | d = \|B^{2j} − u\| → r2 | |
| 89 | t1 = r · d | **corr = t1 >> j** (90) |
| 91–97 | converged := corr < B^{j+1}; neg: overshoot := r·B^j ≤ corr; r' = r·B^j ∓ corr; converged → r ← r', else r ← r' >> j and repeat | |

Write ρ = B^{take} / Q_t ∈ (1, B] (ρ ≈ B / q_top, q_top the top limb of Q). The exact real target of the step is
R_{2j} = B^{2j} ρ; r' approximates it. Let E = R_j − r be the input error (units of r's last limb), x = Q_t r / B^{take−j}
(rational), θ = x − ⌊x⌋ ∈ [0, 1) the fraction the floor of u drops, φ ∈ [0, 1) the fraction corr's floor drops. Then, exactly
(the algebra: d = B^j E/ρ + θ, corr = B^j E − E²/ρ + rθ/B^j − φ, r' = r B^j + corr):

  **r' = R_{2j} − E²/ρ + rθ/B^j − φ, with 0 ≤ rθ/B^j < ρ.**   (1)

So the un-cut code's own step has an error of up to ρ + 1 units (the floor of u costs up to ρ units of r'), the steady state
of the chain is |E| ≤ ρ + 1 (E' = E²/ρ − ρθ + φ maps (−ρ, ρ + 1] into itself), and the final μ is off by O(ρ) units.
`t_newton` measures this: |μ − ⌊B^{n_Q+k}/Q⌋| ≤ 8 units at every size and generator (the test's bound; measured). The
division tolerates it: X = ⌊A_h μ / B^{k+1}⌋ with A_h < B^k (A_h has exactly k limbs, `newton_db_divmod_shifted` 253:
sn + dl + 1 − n_Q = k), so an error Δμ moves X by at most ⌈Δμ / B⌉ + 1, absorbed by the ±Q corrections (≤ 64, abort beyond:
the digits are exact whenever the run completes — the corrections make X exact; the only failure mode is the abort).

##### 1.2 The cut

`mul_grid` (rns_dist.c 866) with `lowcut = c` skips exactly the pieces (i, l) with oa + ob + la + lb ≤ c (`grid_piece_skipped`
851); a skipped piece is a_i b_l B^{oa+ob} < B^{oa+ob+la+lb} ≤ B^c; the formed pieces are accumulated exactly (full-length
shifted adds), and a product that fits one plane is formed whole whatever the cut. Hence

  t1_cut = t1 − S,  0 ≤ S < n_skip · B^c,  n_skip ≤ 32 · 32.   (2)

The band read is t1 >> v with v = take − j (product 1) or v = j (product 2). Cut at **c = v − g with g ≥ 1 guard limbs**:
S < n_skip B^{v−g} ≤ (n_skip / B^g) B^v < B^v, so

  ⌊t1_cut / B^v⌋ = ⌊t1 / B^v⌋ − δ,  δ ∈ {0, 1},  and δ = 1 only when frac(t1 / B^v) < S / B^v < n_skip / B^g.   (3)

(With g = 0 the same holds with δ ≤ n_skip; the guard makes δ ≤ 1 unconditional and, since frac(t1/B^v) is
equidistributed, makes δ = 1 an event of probability < n_skip / B ≈ 10⁻¹⁵ per product — in practice u and corr are
**bit-identical** to the un-cut ones. The guard costs at most one piece per product, when a piece ends exactly in
(v − g, v]; the model finds no such case at 4·10¹⁰ … 1.3·10¹¹.)

##### 1.3 One step under the cut

Product 1 cut: u_cut = u − δ₁ ⇔ the floor of u dropped θ + δ₁ instead of θ. Product 2 cut: corr_cut = corr − δ₂. So by (1)

  **r'_cut = R_{2j} − E²/ρ + r(θ + δ₁)/B^j − φ − δ₂ = r'_exact + (rδ₁/B^j − δ₂),  |r'_cut − r'_exact| ≤ ρ + 1 ≤ B + 1.**   (4)

The cut adds an error of the same kind and the same size as the floor the code already makes at line 85 (one more unit of
"θ"); it never changes which limbs are formed exactly above the cut (no carry from the skipped region can reach the band:
the skipped mass is < B^v in total, and (3) bounds its effect on the band by one unit). The sign branch (neg), the sign
change when d ∈ {0, 1}, and u = B^{2j} exactly are covered by (4) (d_cut = d ± δ₁ enters (1) through θ).

##### 1.4 The decisions the step takes, and the direction each can flip

- **converged** (corr < B^{j+1}): corr_cut ≤ corr, so it can flip only from repeat to converged (when corr = B^{j+1} exactly
  and δ₂ = 1): never an extra product; an accepted r' then has |E'| ≤ B, which the next step's repeat test catches (its
  corr ≈ E' B^j ≥ B^{j+1}) or, at the last step, costs Δμ ≤ B → ΔX ≤ 2.
- **overshoot** (neg: r B^j ≤ corr): corr_cut ≤ corr, so it can flip only from overshoot to not: r'_cut = r B^j − corr_cut
  would be 1 when r B^j = corr and δ₂ = 1 — a Newton iterate of 1 does not recover. **Closed in the code**: under the switch
  the test is r B^j ≤ corr_cut + 1 (`shifted_cmp_le`'s cut form), which contains every exact overshoot (corr ≤ corr_cut + 1)
  and adds one only when r'_cut would be 1, where shrinking r and retrying (the code's overshoot path) is right.
- **the chain** (jn, take, the shifts by 2j − jn, j − k) does not depend on the values.

##### 1.5 Healing and the final μ

The cut acts only where a product is a grid: the top m doublings (m = 1 at 4·10¹⁰ … 7.6·10¹⁰, 2 at 10¹¹, 3 at 1.3·10¹¹,
modelled from the code's grids). Write x = E/ρ. Exact steps map x ∈ (−1, 1] into itself; a cut step maps x ↦ x² − (θ + δ₁)
+ (φ + δ₂)/ρ, so after m cut steps from |x| ≤ 1 + 1/ρ: x ∈ (−2, 1 + 3/ρ], then (−2, 4 + 6/ρ], then (−2, 16 + 90/ρ]:

  **|μ_cut − μ_exact| ≤ (16 ρ + 90) · B^{−s} + 1 (s = 2j − k ∈ {0, 1}, the last shift) — worst case; ≤ 1 + (ρ + 1) B^{−s} when
  every δ = 0 (probability > 1 − 3 · 10⁻¹⁵).**

Then ΔX ≤ ⌈Δμ / B⌉ + 1 ≤ 2 for ρ ≤ B/32 (q_top ≥ 32); for a Q whose top limb is < 32 (decimal: N! with 18m + 1 digits, 1/18
of the sizes) ΔX ≤ 17 + 1 in the worst case — still inside the 64-correction guard, and the run's `divmod(dev) ...
corrections (dx)` line shows the count. **Conclusion: the digits are unchanged (the corrections make X exact; no path
produces a wrong X silently); the correction count can change by at most +2 in practice, and by ≤ +18 in the worst case.**

##### 1.6 What is needed beyond the cut

- one guard limb (`NEWTON_RECIP_GUARD`, default 1); the cut only when v > g;
- the overshoot test in its cut form (1.4);
- nothing else: no extra corrections, no re-normalisation (u and corr are views normalised by `db_norm` already).

##### 1.7 The mn tier (`recip_mn`)

`rns_mul_dist_mn_cut(C, A, B, G, lowcut, highcut)` applies the same `grid_piece_skipped` rule over shares (a one-plane product
is skipped only when all of it is below the cut, which cannot happen for v ≤ take + j); `mdb_shift(&u, &t, take − j, …)` and
`mdb_shift(&corr, &t, j, …)` are the views. The same argument applies limb for limb; the overshoot test `mdb_cmp(&rs, &corr)`
gets the same +1 form.

(The code, the tests and the models follow in §2–§4; RESUME at the end.)

#### 2. E7 — the code and the tests

**Code** (commits 47987dc, 34a9588): `newton_db.c` — `newton_recip_cut(v)` (the low cut for a product read as t1 >> v: v − guard, 0 when
off or v ≤ guard); `recip_db2` forms Q_t·r with `rns_mul_high_db(…, take − j − guard)` and r·d with `rns_mul_high_db(…, j − guard)`
(`rns_mul_high_db(…, 0)` is `rns_mul_dist_db`: the same call when off); `recip_mn` the same through `mn_prod_cut`; the overshoot
test in its cut form (`shifted_cmp_le(…, plus1)` / the `rs − corr == 1` check over shares); a per-doubling line `newton(db) j … recip
cut N pieces skipped, M formed` for the doublings whose products are grids (counters `n_formed` / `n_skipped` added to `struct
rns_dist_stats`, rns_mul.h — one line, summed in `mul_grid`). `mn_plan.c`: the plan printer passes the same cuts. README: two rows
(`NEWTON_RECIP_CUT`, `NEWTON_RECIP_GUARD`).

**Plan printer** (login node; the code's decisions on predicted sizes): `NEWTON_RECIP_CUT=1 MN_PLAN_ONLY=100000000000:1 ./ecalc`:
at 10¹¹ the top doubling's Q_t·r 9 × 3 grid forms 24 instead of 27 pieces, r·d 2 × 4 forms 7 instead of 8 (recip pieces 101 → 97);
at 1.3 × 10¹¹: 24 → 21 and 12 → 9, plus 6 → 5 one doubling lower (103 → 97). At 4 × 10¹⁰ and 7.64 × 10¹⁰ nothing is skipped
(the grids are k × 1 or 2 × 2 with pieces that all reach the band). `mn_model.py --e7` reproduces every count.

**Node tests** (job 21129, s24-30, 23:55–00:05 CDT, `~/R114_batch.sh`, logs `~/R114/*.log` on aac6; all **measured**):

| test | command (on the node) | result |
|---|---|---|
| t_newton off | `NEWTON_DEVICE=1 tests/t_newton 20` | VERIFY OK (620 checks), 29 s |
| t_newton cut | `NEWTON_DEVICE=1 NEWTON_RECIP_CUT=1 tests/t_newton 20` | VERIFY OK (620 checks), 28 s (no grid at these sizes: inert) |
| t_newton cap 2²⁰ off / cut / cut guard 0 | `… DIST_LOGN_TEST=20 [NEWTON_RECIP_CUT=1 [NEWTON_RECIP_GUARD=0]] tests/t_newton 20` | VERIFY OK (620 checks) each, 28–29 s; every reciprocal ≥ 2²⁰ limbs is a grid (1386 grid products in the cut run; pieces skipped at the top doublings, e.g. j 1048577 → 2097154: 1 of 12 and 1 of 6); the reciprocal error against GMP's floor **+0 at every printed size**, as without the cut; the divmod corrections within the test's 4 |
| e9 decimal / binary, real cap | `NEWTON_RECIP_CUT=1 [LIMB_BASE=2] ./ecalc 1000000000` | VERIFY OK, **identical** (sha1 bf9f4b08…) to ref/e_1000000000.txt; 11.0 / 25.3 s total |
| e9 decimal / binary, cap 2²⁴ (control off + cut) | `… DIST_LOGN_TEST=24 [NEWTON_RECIP_CUT=1] [LIMB_BASE=2] ./ecalc 1000000000` | VERIFY OK, **identical** in all three; the cut skips pieces at the top doubling in both bases |
| 10¹⁰, cap 2²⁸, cut | `NEWTON_RECIP_CUT=1 DIST_LOGN_TEST=28 ./ecalc 10000000000` | VERIFY OK, **identical** to results/e_1e10.out (sha1 3fc02547…); corrections 0/0; 33.0 s total (the grids are k × 1: nothing skipped, the switch's path taken) |
| 4 × 10¹⁰, real cap, cut | `NEWTON_RECIP_CUT=1 ./ecalc 40000000000` | VERIFY OK, **identical** to results/e_4e10.out (sha1 43c84e49…); 64.6 s total (dm 23.6, recip 10.3); corrections 0; the 5 × 1 and 1 × 3 grids skip nothing, as modelled |

The job cancelled itself at 00:04:42 CDT (batch 546 s). A 10¹¹ run with pieces skipped at the real cap is W2's measurement.

**Modelled saving** (`mn_model.py --e7`, D2's pipeline law on the exact grids; the study's "−5…−15 s" was assumed):

| digits | grid doublings' products, whole → cut | saving | of the modelled reciprocal | of dm |
|---|---|---|---|---|
| 4 × 10¹⁰ | 8.24 → 8.24 s | 0 | 0 % | 0 % |
| 7.64 × 10¹⁰ | 19.4 → 19.4 s | 0 | 0 % | 0 % |
| 10¹¹ | 43.0 → 39.1 s | **3.9 s** | 6.8 % (56.6 s) | 3.5 % (110 s) |
| 1.3 × 10¹¹ | 64.7 → 56.0 s | **8.7 s** | 10.1 % (85.4 s) | 4.6 % (188 s) |

The cut skips only pieces that contain none of r's top part: a k × 1 grid (r whole in every piece) skips nothing, and the band is 2/3
of Q_t·r and 1/2 of r·d, so the lower-left triangle is 1/9–1/4 of the pieces where the grid is at least 2 × 2 on r. Real, small, free.

#### 3. E9 — Karatsuba over the device grid (model; `mn_model.py --e9`)

Model: `kara_product` puts up to two 2 × 2 Karatsuba layers above `mul_grid` (z0 = A0B0, z1 = (A0+A1)(B0+B1), z2 = A1B1 at h =
⌈max(na, nb)/2⌉; a 2 : 1 product as chunks of the shorter operand; each half product carries the band the cuts need of it, which
for a half-cut product includes the low parts the plain grid skips — z1 − z0 − z2 needs them); each piece under D2's pipeline law
(FITTED), the layer's six add passes at the grid's accumulation rate 0.079 s per 2³¹ limbs (ASSUMED), the extra memory = z1's buffer
+ the two half-sums (2 × the operand's length). Also a **cut-aware grid** (split_grid choosing the grid by the cost of the formed
pieces only, the same weights) as the no-memory alternative.

| digits | dm (model) | the big products, plain | cut-aware grid | Karatsuba 1 layer | 2 layers | extra memory (1 layer) |
|---|---|---|---|---|---|---|
| 7.64 × 10¹⁰ | 58.7 s | 51.1 s | 54.1 (+5 % of dm: worse) | 42.2 (**−15 %**) | 36.0 (−26 %) | 68 GB |
| 10¹¹ | 110 s | 100.0 s | 100.0 (0) | 82.8 (**−16 %**) | 73.8 (−24 %) | 89 GB |
| 1.3 × 10¹¹ | 188 s | 175.0 s | 175.0 (0) | 131.5 (**−23 %**) | 122.6 (−28 %) | 116 GB |
| 576 share (7.64 × 10¹⁰, size-1 shapes) | 58.7 s | 51.1 s | 54.1 | 42.2 (−15 %) | 36.0 (−26 %) | 68 GB |

Where the gain is: the reciprocal's 2 : 1 product (9 × 3 = 27 pieces → 12 at 10¹¹) and the division's two half-cut products only
through their full quadrant (7 × 7 with 28 formed → 22). The cut-aware grid does not help: the pieces the grid would skip more of
are the 3·2²⁹ B pieces whose per-point weight is already the lowest.

**Verdict**: on time the model qualifies at the threshold (≥ 20 % of dm) only with two layers, or with one layer from 1.3 × 10¹¹; but
**the memory disqualifies it where it would matter**: +68 GB at the 576 share (452 → 520 GB per node: does not fit), +89 GB at
10¹¹ on one node (431 → 520 GB: does not fit), +116 GB at 1.3 × 10¹¹. A leaner implementation (the half-sums formed per piece
inside the grid, z1 corrected in place from C) needs ≈ 1 operand + one piece ≈ 61 GB at 10¹¹ — still not inside the ceiling's
margin. So E9 is a time item for runs well below the ceiling only (≤ 8 × 10¹⁰ on one node: −15 % of dm ≈ −5 % of the wall) and
**not for the 576 target**. Recorded; not implemented in W2 unless the user wants the below-ceiling time gain (≈ 1 day, F2's estimate).

#### 4. E10 — the 576-node tree-level spill (model; `mn_model.py --e10`)

Model (`tree_need_e10`, `node_peak_e10`, `ceiling_e10`): mem_model's per-node terms (the calibrated formulas of the code) with the
tree's heavy levels (the two 3-way levels of the 576 schedule, 192 and 576, whose per-node live sets are equal) under three
variants of `mn.c tree_level_k`'s live set — **code** (both pairs of the child, the running pair, the new pair live across the level's
two products), **free** (E10a, no disk: P_i and P_run are dead after P_n = P_i Q_run + P_run and freed before Q_n = Q_i Q_run; the
peak becomes max(2c + 2r + n, c + r + 2n) instead of 2c + 2r + 2n), **spill** (E10b = apumult M56: free + Q_i spilled under the first
product and P_n under the second, each read back: c + n bytes per node per level exposed at the disk rate; the writes hidden under
the level's products — ASSUMED); × the dm need as built (V0) and as F1 §6 models V2 / V3 (ASSUMED as the ratios 270/309, 243/309);
× `MN_T_CHUNK_MB` 0 / 1024 (the built switch). The ceiling per node at 480 (502) GB by bisection; total digits with the top node's
share 1.036 × the average (D213d); the wall from `mn_model.run` at the ceiling + the spill's I/O; elasticity against the built default's
480-GB point (8.09 × 10¹⁰ per node, 4.50 × 10¹³, 278 s).

| chunk | dm | tree | 480 GB: digits | wall + I/O at 5 / 10 GB/s | elasticity 5 / 10 GB/s | 502 GB: digits |
|---|---|---|---|---|---|---|
| off | V0 | code (built) | 4.50 × 10¹³ | 278 s | — | 4.79 × 10¹³ |
| off | V0 | free / spill | 4.50 × 10¹³ | +0 / +23 s | — (the dm need binds: no digits) | 4.79 × 10¹³ |
| 1024 | V0 | code | 5.01 × 10¹³ | 323 s | 1.41 | 5.33 × 10¹³ |
| 1024 | V0 | free / spill | 5.01 × 10¹³ | +0 / +26 s | 1.41 / 2.2 | 5.33 × 10¹³ |
| off | V2 | code | 4.73 × 10¹³ | 298 s | 1.42 | 5.07 × 10¹³ |
| off | V2 | **free (all heavy levels)** | **5.18 × 10¹³** | 351 s | 1.71 | 5.52 × 10¹³ |
| off | V2 | spill (all heavy levels) | 5.18 × 10¹³ | 351 + 64 / 27 s | 3.2 / 2.3 | 5.52 × 10¹³ |
| 1024 | V2 | code | 5.33 × 10¹³ | 382 s | 2.00 | 5.70 × 10¹³ |
| 1024 | V2 | **free** | **5.76 × 10¹³** | 419 s | 1.80 | 6.15 × 10¹³ |
| 1024 | V2 | spill | 5.76 × 10¹³ | 419 + 71 / 29 s | 2.7 / 2.2 | 6.15 × 10¹³ |
| 1024 | V3 | free | 6.34 × 10¹³ | 470 s | 1.68 | 6.79 × 10¹³ |

(The full table, 48 rows, is the command's output; the walls carry the model's grid steps, e.g. 323 → 382 s between 9.0 and 9.6 × 10¹⁰
per node.)

**Verdict**: (1) the **disk spill (M56) is never the right lever at 576**: as built (V0) the dm need binds and it buys no digits; once V2
lowers the dm need, the no-disk **early free** reaches the same ceiling (the dm need binds again right after), and the spill only adds
27–70 s per node of exposed read-back — elasticity 2.2–3.2 at 5 GB/s, 1.9–2.3 at 10 GB/s, against the 1.4–1.8 of the alternatives
and the user's 1.6 threshold. (2) The **early free (E10a)** — `tree_level_k` freeing the dead P_i / P_run shares between the level's two
products, and `binsplit.c tree_need_dev` (the arena formula, mirrored in `mem_model.tree_need_dev`) counting max(2c + 2r + n,
c + r + 2n) instead of 2c + 2r + 2n — is bit-identical, costs nothing, and is worth +9.7 % digits at 576 once V2 (L1's E2) is in
(4.73 → 5.18 × 10¹³ at 480 GB; with `MN_T_CHUNK_MB=1024` 5.33 → 5.76 × 10¹³) at elasticity 1.7–1.8 (all of it compute at the
larger size). (3) `MN_T_CHUNK_MB=1024` alone stays the best single lever (5.01 × 10¹³, elasticity 1.41). Recommendation for W3:
implement E10a (two small edits, in mn.c and binsplit.c's `tree_need_dev`, plus the model), not E10b.

#### RESUME (updated 01:20 EDT)

**Done in W1**: E7 designed (§1), implemented (`NEWTON_RECIP_CUT`, `NEWTON_RECIP_GUARD`; commits 47987dc, 34a9588 on `p14-R1`),
gates passed on s24-30 (§2: t_newton 20 in five variants, e9 both bases at the real cap and at cap 2²⁴, 10¹⁰ at cap 2²⁸ and
4 × 10¹⁰ at the real cap identical to the references). E9 and E10 modelled (§3, §4) with verdicts. aac6 clone `~/ntt-R114` on
`p14-R1` (bundle-delivered; check `git log --oneline -1` there against this branch's head).

**Next (W2)**: (1) E7 measured at 10¹¹ on one node: `NEWTON_RECIP_CUT=1 RNS_VERBOSE=1 ./ecalc 100000000000 <out>` against a run
without the switch on the same node (no stored 10¹¹ reference: sha1 of the two outputs; expect −3.9 s of the reciprocal, 4 pieces
skipped at the top doubling, corrections unchanged), reading the per-doubling `recip cut` lines; (2) E9: not implemented (the memory
verdict, §3) unless the integrator asks for the below-ceiling time item; (3) E10a (the early free) as a W3 code item if adopted.
Nothing of mine running or pending on aac6.

#### 6. DB_POOL_VMM — the VMM-backed block pool (branch `p14-R1v` = p14-L1 669ba1e + main 4bc48ee; commit 460de29)

**Design as built** (dbig.c, the `db_vmm_*` block; binsplit.c `arena_get` / `binsplit_release_arenas` / the seed stream; mem.c `mem_dev_note`):
one VA range per APU reserved at 3 × the arena (`DB_POOL_VMM_RESERVE`), the arena mapped from its start in 2 GiB chunks
(`DB_POOL_VMM_CHUNK_GB`; `hipMemCreate` + `hipMemMap` + `hipMemSetAccess` for the four APUs, then a memset as `mem_dev_alloc` does),
registered as one borrowed region of the pool (the two parity halves that `donate_one` hands over later join it: `db_donate_adjacent`
recognises a range inside the VMM region). `q_alloc_locked`: when `ext_take` finds no extent, `vmm_make_room` collects the wholly free
mapped chunks (a chunk inside one free extent), unmaps them (removing them from the extents), finds the lowest run of m = ⌈need/C⌉ empty
slots, maps the same handles there (no create, no page work) plus new chunks for any remainder (a "VMM growth" line: the only in-phase
growth, never a hipMalloc), sets access, inserts the m·C extent and retries `ext_take`. If the VA has no run of m empty slots the
chunks go back and the old hipMalloc fallback runs (counted as before). The seed thread (`binsplit.c`, the stream) never stores into
a VMM range: under the switch every chunk goes through its two host buffers and DMA. Release at `rns_shutdown`: unmap, release, VA freed.

**Gates** (this section is filled by the node batches below; RESUME at the end).

**The host-access rule, found the hard way** (four builds; every one a segfault on the first host touch of the VMM arena): besides the seed
stream, the bs batch tier does three things on the host over the region pools that hipMalloc memory allowed — the stripe-spill merge of the
batch-local tier (`spill_merge`, rns_mul.c), the same merge in the general batch path, and the node lengths after a level (`limb_norm` over
the outputs, binsplit.c). Under the switch: the merges are a device kernel (`k_spill_merge`, one thread per product, the same arithmetic),
the lengths come by a DMA of the top 64 limbs (`node_norm`), the seed table by `region_copy` (DMA). `rns2_mul_batch` (the binary E2 tier) and
the CPU schoolbook tier (`BS_SCHOOL_NL`) still write pool memory from the host and are not supported under `DB_POOL_VMM` (not on the
default path). Commits 460de29 … a9cfe60.

**Gates** (job 21213, s24-16, 16:29–16:41 CDT = 17:29–17:41 EDT; `~/R1v_gate.sh`, logs `~/R114/*.log`; all **measured**, digits by sha1):

| test | switches | result | in-phase hipMalloc | VMM remaps |
|---|---|---|---|---|
| t_newton 20 | `NEWTON_DEVICE=1 DB_POOL_VMM=1` | VERIFY OK (620 checks) | 0 | 0 |
| e9 decimal / binary | `DB_POOL_VMM=1` | **identical** both | 0 | 0 |
| e9 decimal / binary | `DB_POOL_VMM=1 DM_TIGHT=1` | **identical** both | 0 | 0 |
| 4 × 10¹⁰ | `DB_POOL_VMM=1` | **identical** to results/e_4e10.out | 0 | 0 |
| 4 × 10¹⁰ | `DB_POOL_VMM=1 DM_TIGHT=1` | **identical**; total 72.4 s (dm 22.9) | **0** (L1: 4.44 GB × 4 APUs) | 4 |
| 10¹¹ | `DB_POOL_VMM=1 DM_TIGHT=1` | VERIFY OK; total 229.8 s (bs 96.9, dm 102.9, recip 43.5) | **0** (L1: 22.22 GB on 2–3 APUs) | 3: `APU1 VMM remap for a 22.22 GB request: 11 free chunks moved to slot 30 (23.62 GB contiguous) in 0.434 s; free 31.09 GB in 3 extents, live 33.33 GB` (APU2 0.47 s, APU3 0.49 s) |

The remap is the case L1 hit (t, 22.2 GB, with 31 GB free in 3 extents): 11 wholly free 2 GiB chunks are unmapped and mapped again
contiguously in ≈ 0.45 s per APU (no page work: the same physical chunks), no growth at all. 1.4 × 10¹¹ and the timing pairs: the next batch.

**1.4 × 10¹¹ at cap 2³¹, `DB_POOL_VMM=1 DM_TIGHT=1`** (job 21214, s24-16, 16:42–16:50 CDT; **measured**): **VERIFY OK, total 485.2 s** (init 44.4,
bs 182.8, dm 257.8 of which recip ≈ 115), **zero in-phase hipMalloc, 7 remaps, 0 growths**: the division's t (31.11 GB per APU) found 43.5 GB
free in 5–6 extents and was served by moving 15 free chunks (32.2 GB contiguous) in 0.64–0.69 s per APU, twice on three APUs. Device
466.6 GB in use at the peak (per APU 116–118 GB; block pool 362.9 GB borrowed, peak live 326.7 GB) — the run L1's batch 5 lost to
"t 2.6 GB short, the driver refused the fallback" now completes: **beyond the 1.30 × 10¹¹ ceiling of the built code.**

**Timing at 10¹¹, same node (s24-16, job 21214), two runs each (measured):**

| run | total | init | bs | dm (recip) | remaps |
|---|---|---|---|---|---|
| default 1 / 2 | 212.1 / 213.7 s | 21.1 / 23.6 | 88.0 / 86.9 | 102.2 (45.6) / 102.4 (45.6) | — |
| `DB_POOL_VMM=1 DM_TIGHT=1` 1 / 2 | **228.3 / 227.2 s** | 29.4 / 28.9 | 96.3 / 95.2 | 101.8 (44.4) / 102.2 (44.7) | 3 / 3 |

Tight + VMM costs **+15 s (+7 %) at 10¹¹**, none of it in dm (identical; the three remaps take 1.4 s in total). It is init (+8 s: the VMM
mapping of the 260 GB arena runs at 0.18–0.31 s/GB with the four APUs mapping at once — `hipMemCreate` serialises in the driver like
hipMalloc but the seeds waited 18.5 s for the regions instead of 9.8) and bs (+8 s: the seeds now go through the two host buffers and DMA,
7.7 s issuing instead of direct stores; plus the per-level device merges and DMA lengths). Both are outside the digits' path and both
have obvious remedies (map the arena in the seed thread's shadow as `binsplit_pregrow` did for hipMalloc; a larger seed buffer): W3 items.
Device at 10¹¹: 260 GB of regions against the default's 310 (L1's −50 GB), with no fallback.

**Verdict**: the gate is met — bit-identical digits (t_newton, e9 both bases, 4 × 10¹⁰ = results/e_4e10.out) with `DB_POOL_VMM=1` alone
and with `DM_TIGHT=1`; zero in-phase hipMalloc at 4 × 10¹⁰ and 10¹¹ under both; 1.4 × 10¹¹ at cap 2³¹ VERIFY OK in 485 s. Cost +7 % at 10¹¹
today, reducible.

#### RESUME (updated 18:15 EDT)

**Done**: `p14-R1v` (= p14-L1 669ba1e + main 4bc48ee + the pool: 460de29 … a65a3eb + this commit) builds and passes every gate the integrator
set (§6). aac6 clone `~/ntt-R114` on `p14-R1v`; logs in `~/R114/` (gate_summary.txt, time_summary.txt, e14_vmm_tight.log, e11_*.log).
No job of mine running or pending (21210–21214 all cancelled by their scripts).

**Open / next**: (1) the +8 s of init (map the VMM arena under the seed thread as the hipMalloc arena was; or map with one thread per APU
earlier) and the +8 s of bs (the seed DMA path; a larger `BS_SEED_CHUNK_MB`); (2) not supported under `DB_POOL_VMM`: the binary E2 tier
(`rns2_mul_batch`) and the CPU schoolbook tier (`BS_SCHOOL_NL`) — they write pool memory from the host; (3) `T_ALLOC_VMM_SERIAL=0`
untested; (4) the reserved VA is 3 × the arena: a run that remaps more than 2 × the arena in whole chunks would fall back to hipMalloc
(counted as before) — not seen (7 remaps at 1.4 × 10¹¹ used 15 slots past the end each time; the slots are reused once the holes coalesce).

##### 6b. Cutting the +15 s (W3 evening)

**Where it was** (job 21223, 1e11, s24-26; measured): the seed thread is the critical path: default `waited 14–16 s for the regions, spans
22.5` → done at 37–39 s; tight+VMM `waited 19–21, spans 22, DMA 1.7, issuing 7.9` → done at 52 s (init 32.5 vs 27–30, bs 95 vs 85; dm identical).
Init-only experiments (job 21217): `hipMemCreate` maps at 0.077 s/GB aggregate against hipMalloc's 0.036–0.05 whatever the chunk size
(2 / 8 / 32 GiB: 22.3 / 25.0 / 24.7 s for 262 GB against 10.9 s); the seed DMAs contend with the plane pools' hipMalloc for the runtime lock
(issuing 7.4–8.3 s for 24–48 chunks).

**Steps**: (1) 1cb05f7 — the arena's parity-0 half (the seeds' target) mapped at init, the rest by a thread per APU, waited for at level 1
(`pool_get(1)`) / the donations / release; the seed chunk 4 GB under the switch. (2) 143697d — the threads start only after init's plane
pools (`db_vmm_bg_release` from `rns_init`), so the seeds get their half first and the pools their turn. Result (job 21223): no gain
(230.3 / 229.7 vs 218.2 / 215.4 s) — `region pools` still 22.6 s: `binsplit_pregrow`'s own sizing `pool_get(1, …)` triggered the wait and
so the whole mapping inside init. (3) 2194fd0 — pregrow's pool_get exempt from the wait. Measured next (job below).

Job 21224 (2194fd0, s24-16, measured): init 19.0 / 19.4 s (**−8 s against the default's 27.3 / 29.4**: region pools 9.8 s = the half), but the seed
thread's DMA issuing grew to 15.7 s (24 issues against the plane pools and the background mapping): the seeds done at 47 s against 38–40, bs
103 vs 86, total 226.5 / 226.6 vs 217.5 / 218.8 (+9 s, +4 %). (4) 6190d5d — the seed chunk 8 GB under the switch (12 issues; +12 GB of pinned host
memory during the seeds — at 1.4 × 10¹¹ the node is then at ≈ 497 GB during init, below 502 but above the 480 margin: to watch).
Job 21227 (6190d5d, measured): 224.5 / 225.6 vs 217.9 / 218.5 s (+7 s, +3.2 %); init 18–19 vs 27–29; the seeds' issuing still 16–17 s with 12
chunks — 8 GB per 1.36 s = **6 GB/s, i.e. `hipMemcpyAsync` into a VMM range runs synchronously at the staged-copy rate**, not lock waits.
(5) 4009662 — the seeds' chunks copied by a kernel on the device's own non-blocking stream (`db_copy_h2d_async`: pinned host memory read over the
fabric; the call returns at once, the wait before the buffer is reused).
Job 21229 (4009662, measured): 225.0 / 223.3 vs 214.1 / 215.5 s — issuing still 16–17 s with the kernel launches, so the blocking is the
runtime lock, not the copy: the four background mappers hold it while blocked on each other in the driver (their chunk calls ran 13 s
concurrently with the seeds). (6) 2e7d2e1 — the background mappers one at a time; the seeds' longest issue printed.

**Result** (job 21231, 2e7d2e1, s24-16, back to back, measured):

| run | total | init | bs | dm | the seed thread |
|---|---|---|---|---|---|
| default 1 / 2 | 213.8 / 215.6 s | 24.3 / 23.5 | 85.5 / 88.4 | 103.2 / 103.0 | done at 32.9 / 35.1 s (waited 9.8 / 11.9 for the regions, spans 22.5) |
| `DB_POOL_VMM=1 DM_TIGHT=1` 1 / 2 | **217.2 / 216.6 s** | 19.0 / 19.7 | 94.3 / 93.9 | 103.2 / 102.1 | done at 37.9 / 38.8 s (waited 4.9 / 5.5, spans 21.5, issuing 9.3 of which one launch 8.1) |

**+2.2 s = +1.0 % at 10¹¹ (was +15 s, +7 %)** — inside the ~2 % goal. e9 both bases and 4 × 10¹⁰ identical with both switches (job 21231; 4 × 10¹⁰
tight+VMM 66.8 s). What remains is one 8-s wait of the seed thread's first launch behind a plane-pool hipMalloc (12 GiB, the runtime lock); the
init is now 5 s *faster* than the default (the arena's second half maps under the seeds and bs), dm unchanged. Steps 1–6 in the commits
1cb05f7 … 2e7d2e1; every step gated (e9 both bases, 4 × 10¹⁰ identical).

#### RESUME (updated 23:35 EDT)

**Done**: DB_POOL_VMM built, gated and its overhead cut to +1 % at 10¹¹ (§6, §6b). `p14-R1v` head = this commit; aac6 clone `~/ntt-R114` on
it; no job of mine running or pending (21224–21231 all cancelled by their scripts). Logs `~/R114/gate2_summary*.txt`, `e11_*.log`.

**Open**: (1) the seed thread's single 8-s launch wait behind a plane-pool hipMalloc (an ordering trick: issue the first chunk before the
pools, or split the 12 GiB pool allocations) — ≈ −2 s more; (2) the seed buffers are 2 × 8 GB pinned under the switch (host HWM 27 GB at
10¹¹; at 1.4 × 10¹¹ the node sits ≈ 497 GB during init: to re-check with a 1.4 × 10¹¹ run at this head); (3) not supported under the switch:
the binary E2 tier and the CPU schoolbook tier (host writes into pool memory); (4) `T_ALLOC_VMM_SERIAL=0` untested.

## DKM15: the division in two quotient halves with a half-length reciprocal (Phase 15 Batch 3, agent DKM)

(former file: DKM15.md)

Branch `p15-DKM` from `int15g` be2eec3. The item is SC's rank 2 (results/TARGET.md §SC15 §3.2), approved by the user on 2026-09-28.
The aac6 clone is `~/ntt-DKM15`. Times are Eastern. Every number is labelled **measured**, **modelled** or **assumed**.

The switch is `NEWTON_DKM=1`. It is off by default. With it off, the code paths are the ones on the base, unchanged.

#### 1. Design note (stage 1, written 2026-09-28 17:50–18:40 EDT)

##### 1.1 Today (`newton_db.c` on be2eec3)

The division computes X = floor(A / Q) with A = S·B^dl, S = P + Q, Q of n_q limbs, na = S.n + dl, and k = na − n_q + 1.
At the target, k ≈ dl ≈ n_q ≈ n = 2.83 × 10¹² limbs.

1. **The reciprocal.** `recip_db2` (size 1) or `recip_mn` (size > 1) computes mu ≈ B^(n_q+k)/Q to k limbs, with k + 1 limbs in all.
   It uses the anchored doubling chain k, ⌈k/2⌉, …. The last doubling (j = n/2 → n) forms two products: Q_t·r, a middle product
   under `NEWTON_RECIP_MID`, and r·d, cut below j under `NEWTON_RECIP_CUT`.
2. **The quotient estimate.** X₀ = ((S >> (n_q − 1 − dl))·mu) >> (k + 1). This is a high product, n × n, with the pieces below
   k + 1 skipped (`NEWTON_HIGHPROD`).
3. **The remainder window.** It forms X₀·Q mod B^w with w = n_q + 2. This is a low product, n × n, with the pieces above w skipped
   (`NEWTON_LOWPROD`). The window is A mod B^w = (S mod B^(w−dl))·B^dl.
4. **The corrections.** They run while R < 0 or R ≥ Q, with at most 64. The early writer starts on X₀ before the low product, through
   `newton_db_x_hook` / `newton_db_x_dev` at size 1 and `newton_mn_x_hook` (`MN_OUT_EARLY`) at size > 1. With
   `ECALC_CORR_PATCH=2`, the corrections are deferred (`newton_x_dx`), and the output stage patches the written tail.
   `ECALC_TEST_CORR=k` moves X₀ by −k before the hook.

The area in units of n², at piece granularity. Each is roughly the part of the a × b rectangle the cuts keep:
- The reciprocal: the last doubling is Q_t·r (the band between n/2 and n of an n × n/2 rectangle, 0.25) plus r·d (0.125). The chain
  adds a factor of 4/3, so ≈ 0.5 in all.
- The high product: 0.5.
- The low product: 0.5.
- The total is ≈ 1.5.

##### 1.2 DKM (GMP's `mu_div` form, divide-and-conquer on the quotient)

Let s = min(⌊k/2⌋, dl), k₁ = k − s, and h = max(k₁, s + 1). With s = ⌊k/2⌋, h = ⌊k/2⌋ + 1. The steps:

- **The reciprocal to h limbs.** mu_h ≈ B^(n_q+h)/Q has h + 1 limbs. The chain stops one doubling earlier.
  - Size 1: `newton_db_recip(k_mu)` computes to h(k_mu), and the division truncates it as it does a longer kept mu today.
  - Size > 1: `recip_mn(h(k_mu))`.
  - Because h(·) is monotone, h(k_mu) ≥ h(k) for the actual k ≤ k_mu.
- **Step 1, the high half.** A′ = ⌊A / B^s⌋ = S·B^(dl−s), since s ≤ dl. This is the shifted form of today with dl₁ = dl − s, and
  step 1 is exactly today's division on (S, dl₁) with k₁ = k − s:
  - X_hi₀ = ((S >> (n_q − 1 − dl₁))·mu₁) >> (k₁ + 1), where mu₁ is mu_h's top k₁ + 1 limbs. This is a high product,
    (k₁ + 1) × (k₁ + 1), cut below k₁ + 1.
  - The window A′ mod B^w = (S mod B^(w−dl₁))·B^dl₁. The low product is X_hi₀·Q mod B^w, k₁ × n_q, cut above w.
  - The correction loop runs to 0 ≤ R₁ < Q and is applied to X_hi in place. There is no hook, no deferral and no test offset at
    this step. The writer never sees X_hi.
  - The result: X_hi = ⌊A′/Q⌋ and R₁ = A′ − X_hi·Q, both exact. S is freed here.
- **Step 2, the low half.** A₂ = R₁·B^s + (A mod B^s) = R₁·B^s, because A mod B^s = 0 when s ≤ dl. Also A₂ < Q·B^s, so
  ⌊A₂/Q⌋ < B^s. This is the shifted form again, with S₂ = R₁ and dl₂ = s:
  - k₂ = R₁.n + s − n_q + 1 ≤ s + 1, and mu₂ is mu_h's top k₂ + 1 limbs.
  - X_lo₀ = ((R₁ >> (n_q − 1 − s))·mu₂) >> (k₂ + 1). This is a high product, (s + 1) × (s + 2), cut below k₂ + 1.
  - If R₁.n + s < n_q, then A₂ < Q: X_lo = 0, and R = A₂, with no product.
- **Assembly.** X₀ = X_hi·B^s + X_lo₀, a shifted add. X_lo₀ can exceed B^s − 1 by a few units, and the add carries into X_hi's
  part. When X_lo₀ < B^s (almost always), the low product reads X_lo₀ as a view of X₀'s low s limbs, and X_lo₀ is freed.
  - `ECALC_TEST_CORR=k` is applied to X_lo₀ before the assembly. It needs X_lo₀ ≥ k for k > 0. X_lo₀ is ~s limbs, so this always
    holds at real sizes; the code stops with a message otherwise.
  - Then comes today's hook: the writer, and the residues at size 1.
- **The remainder.** The low product is X_lo₀·Q mod B^w, s × n_q, cut above w. The window is A₂ mod B^w = (R₁ mod B^(w−s))·B^s.
  - Today's correction loop and the output of dx are unchanged: deferred under `ECALC_CORR_PATCH`, in place otherwise, and R's
    residues come back as today.

**Exactness.** Each step is today's Barrett step followed by today's correction loop. The loop stops only when 0 ≤ R < Q, so each
step returns the exact floor and remainder whatever mu's error. mu's error sets only the number of corrections, which is ≤ 2–3 per
step as today, with a fatal stop at 64.
- A = A′·B^s = (X_hi·Q + R₁)·B^s = X_hi·Q·B^s + A₂.
- A₂ = X_lo·Q + R with 0 ≤ R < Q.
- So A = (X_hi·B^s + X_lo)·Q + R with 0 ≤ R < Q, and X = ⌊A/Q⌋ exactly.

The only new arithmetic is the assembly add, which is exact, and the choice of mu's length. A too-short kept mu falls back to today's
fresh reciprocal (`kept_ok`), which is correct but slow; the verbose line says so.

**The area**, in the same units:
- The reciprocal to n/2: 0.125.
- Step 1 high: 0.125. Step 1 low (n/2 × n mod B^n): 0.5 − 0.125 = 0.375.
- Step 2 high: 0.125. Step 2 low: 0.375.
- The total is ≈ 1.125, against 1.5 today (−25 %).
- The division alone rises 1.0 → 1.0, and all the gain is the reciprocal's last doubling. That matches SC's piece model:
  reciprocal 43.8 → 18.2 s, division 50.3 → 53.7 s (cache 2, modelled).

Two things could make the low products cheaper, and neither is in scope:
- **The wrap-around product** of GMP (X_hi·Q mod B^N − 1). The grid has no cyclic primitive.
- **A middle product for R₁'s top.** It is the same area, and it would give up exactness at step 1.

**Why not the host path.** `newton_db_divmod`, the host-A form, is not the production path: ecalc at size 1 uses
`newton_db_divmod_shifted` (`ovl3`). DKM is built into `newton_db_divmod_shifted` and `newton_mn_divmod` only. t_newton tests
`newton_db_divmod_shifted` directly (§1.6).

##### 1.3 The corrections, the patch (`ECALC_CORR_PATCH=2`) and the early writer (`MN_OUT_EARLY`)

- **Step 1's corrections** are applied to X_hi before X₀ exists, so the writer, the patch and dx never see them. They are counted in
  a new field, `newton_st.dkm_corr`, and printed in the verbose line.
- **The final corrections** are today's. They are ± a few on X₀ = X_hi·B^s + X_lo₀, and they are deferred and patched, or applied in
  place (`db_add_small` / `mdb_add_val` on the whole X), as today. The patch zone (4096 digits) and the deferral contract of K15 §2.1
  are unchanged: `newton_x_defer` is read at the same point, and X is not changed after the hook.
- **The hook moves** from before the one low product to before step 2's low product. The writer then overlaps only that product,
  the window and the corrections (≈ 0.375 of the area) instead of today's 0.5. So the with-write gain is about half the no-write
  gain (SC: −16.2 of −31.9 s at cache 0, modelled).
- **A follow-up that would recover it (not built; outside my files).** After step 1's corrections, the top limbs of X are final:
  X = X_hi·B^s + X_lo with 0 ≤ X_lo < B^s exactly, so X's limbs at and above s are X_hi's. A writer hook after step 1 could therefore
  write X_hi's digits (the file's first ≈ half) during step 2's two products, with no patch risk to them. That needs changes in
  `mn_out.c` / `ecalc.c`: part files written in two ranges.
- **`ECALC_TEST_CORR=k`** keeps its meaning: X₀ − k before the hook, then k more final corrections. A second hook,
  `NEWTON_DKM_TEST_HI=k` (a test only, |k| ≤ 60), moves X_hi₀ by −k before step 1's loop. It forces step 1's corrections, which the
  final count must not show.

##### 1.4 Memory (`mem_model.py`)

The per-device sets at the phase's peaks, n = n_Q share:
- **Today:**
  - The reciprocal's last doubling (`dm_layout` v2, DM_TIGHT) is P + Q + r (j + 1) + r2 (2j + 4) + t1 + a piece, with j = n/2.
  - The division's high product is Q + S + mu (k) + t (2k), ≈ 5n.
  - The low product is Q + X + Aw + xq, ≈ 4n.
- **DKM:**
  - The reciprocal's last doubling is at j = n/4, so r2 and t1 are halved.
  - Step 1's high product is Q + S + mu (n/2) + t (n), ≈ 3.5n.
  - Step 2's high product is Q + X_hi (n/2) + R₁ (n) + mu (n/2) + t (n), ≈ 4n. The window A₂'s is formed after it, then R₁ is freed.
  - The assembly is Q + X_hi + X_lo₀ + Aw₂ + X, ≈ 4n, briefly. Then X_hi and X_lo₀ are freed.
  - Step 2's low product is Q + X + Aw₂ + xq, ≈ 4n, as today.

At the target (modelled: `mem_model.mem_per_node(5.1e13/576, 576)`, the launch line), the arena is max(bs 201.8, dm 284.1,
tree 264.2) = 284.1 GB per node. The dm need binds, and its three parts per device are nearly equal:
- v2 (the reciprocal) 56.83 GB;
- v3 (the top bs levels beside the hole) 56.51 GB;
- div 55.45 GB;
- the hole (t / t1's block) 19.98 GB.

The saving comes only through a smaller hole: DKM's t is ≈ k limbs instead of 2k + 8, and t1 is halved. The hole is binsplit.c's
`dm_layout`, the layout the arena is carved to, which is not my file. So:
- **with the code as it is, DKM saves no node memory.** The blocks get smaller, but the arena is still sized for today's hole.
- **With a follow-up in `dm_layout`** (the hole at DKM's sizes when `NEWTON_DKM=1`), the dm need falls to max(v3′, div′), bounded
  below by the tree need. That is at most −19.9 GB per node at the target (modelled; the same bound as M6's).
  `mem_model.dm_layout(dkm=True)` prices it (stage 3), and `--check-c` stays exact because the default is unchanged.

At size 1, 10¹¹, the arena is bound by the bs regions (261.2 of 273.6 GB, modelled), so there is no memory change.

##### 1.5 The model term (`mn_model.py`)

The term is `division_cost` under `MN_MODEL_DKM=1`, an environment knob like `MN_MODEL_CACHE_SLOTS`, so that P24's and MPB's
`Design` edits do not collide with it. It follows §1.2's exact lengths:
- `recip_cost` to h(k_mu);
- A′_h (k₁ + 1) × (k₁ + 1), with the low cut k₁ + 1;
- X_hi·Q, k₁ × n_q, with the high cut w;
- R₁'s top, (s + 1) × (s + 2), with the low cut s + 2;
- X_lo·Q, s × n_q, with the high cut w;
- the shifts: S → A′_h, t → X_hi, the window, R₁ → R₁_top, t → X_lo, the window, and X_hi → X for the assembly;
- the extra small ops: step 1's cmp / sub / corrections, and the assembly add.

The early writer's hidden part becomes step 2's low product plus the rest (`estimate.py`'s overlap). SC's `sc15_km.py` is the
reference: my term must reproduce its −22.1 / −31.9 s within the guard difference (h = ⌊k/2⌋ + 1 against SC's ⌈k/2⌉ + 2).

##### 1.6 The test plan

1. **t_newton section 6** (`NEWTON_DEVICE=1`): `newton_db_divmod_shifted` with DKM off and on against GMP, on random
   (S, dl, Q) at n_q = 2¹⁰ … 2²⁰ (grids at the top under `DIST_LOGN_TEST`), in both bases. It checks X exactly and R's residues
   against GMP's R mod q.
   - k even and odd.
   - S near a multiple, so that R₁ is small and A₂ < Q gives X_lo = 0.
   - Q = B^k, and Q with all limbs B − 1.
   - Forced corrections: `ECALC_TEST_CORR` ±7 and `NEWTON_DKM_TEST_HI` ±7. The final count must be exactly k, and step 1's count
     must be at least |k|.
   - A kept mu that is too short (the fallback).
   - The existing sections must pass unchanged.
2. **Size 1**, both `NEWTON_DKM=0` and `=1`:
   - e9 in both bases against the references;
   - 4 × 10¹⁰ and 10¹¹ through `ecalc/digcmp.sh` (the packed default);
   - forced corrections at e9: `ECALC_CORR_PATCH=1/2 ECALC_TEST_CORR=±7`, `NEWTON_DKM_TEST_HI=7`.
3. **Timing at 10¹¹**: off / on interleaved, ≥ 3 each, both walls (`total` without the file, and the wall with it), the reference
   evicted, on one node and one job.
4. **Size > 1**: `mnaccept --only unit,e9,mn` with `NEWTON_DKM=1` (sizes 2–4), and `ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7` plus
   K15's long-chain cases (c108u / c108d at sizes 2, 4) with the switch on. The same with the switch off, as a sanity check.
5. **The models**: `mn_model` / `sc15`-style estimate at 5.1 × 10¹³ (cache 0 and 2) with `MN_MODEL_DKM=1`, and
   `mem_model --check-c` unchanged.

##### 1.7 Verdict

**Safe.** The exactness rests on today's correction loop per step, which is unchanged. Each step is today's shifted division with
other arguments, and the one new operation, the assembly, is an exact add. The default path is untouched.

**Worth it.** At the target it saves −22…−32 s modelled without the write (−11…−16 s with it), the largest remaining item after
P24, at about a week. At size 1 it can be measured at 10¹¹: the reciprocal is ≈ a third of dm there.

**Caveats:**
- The memory gain needs a `dm_layout` follow-up (binsplit.c).
- The plan printer (`mn_plan.c`, P24's file) does not know the switch. Its piece counts under `NEWTON_DKM=1` would be today's, and
  its prime-bound check only gets safer, because DKM's products are smaller.

#### 2. Stage 2: the size-1 form (the measurements follow in §4)

**Code** (`ecalc/newton_db.c`, `newton.h`, `tests/t_newton.c`; the README row):
- `newton_dkm_on()` / `newton_dkm_set()`: the switch.
- `newton_dkm_h(k) = ⌊k/2⌋ + 1`.
- `newton_db_recip` stops at h when `newton_db_Qd` is set, which is only ecalc's device-flow prewarm; t_newton's direct calls keep
  their k.
- `newton_db_divmod_shifted` dispatches to `divmod_shifted_dkm` when k ≥ 4 and dl ≥ 1. Its helpers:
  - `dkm_est_db`: today's estimate with mu's top as a view.
  - `dkm_window_db`: the window by a device shift. `db_set_shifted_low` copies limb by limb and is meant for a few limbs, but
    DKM's windows are ≈ n/2 limbs.
  - `dkm_corr_db`: today's correction loop.
- The assembly is `db_add_shifted`. X_lo₀ is read as a view of X's low s limbs whenever X_lo₀ < B^s.
- New fields and hooks: `newton_st.dkm_corr` (step 1's corrections) and `NEWTON_DKM_TEST_HI`.
- With the switch off, the only change in the default path is the one `if` of the dispatch.

**t_newton section 6** (run with `NEWTON_DEVICE=1`):
- 4 sizes (n_q = 1500, 2¹⁴ + 3, 2¹⁸ + 5, 2²⁰ + 1) × 7 shapes, off and on, against GMP: X exactly, and R's residues mod three primes.
- Forced corrections: `ECALC_TEST_CORR` ±7, where the final signed count must move by exactly k. `NEWTON_DKM_TEST_HI` ±7, where
  the final count must not move and step 1 must make them.

#### 3. Stage 3: the multi-node form and the models

**Code.** `mn_divmod_dkm` (newton_db.c) is dispatched from `newton_mn_divmod` when dl ≥ 1 and k_mu ≥ 5.
- `recip_mn` runs to h(k_mu); a fresh reciprocal is taken if the actual h needs more.
- Step 1 is on S with dl − s. The window is `mdb_shift(S, −(dl − s), w)`. X_hi is in the basis of its length + 1, which leaves
  room for +1.
- Step 2 is on R₁ with s.
- The assembly is `mdb_shift` × 3 + `mdb_addsub`. X ends in the basis of its length, as today's X.
- `newton_mn_x_hook` is called there. X_lo's low product follows, then `newton_mn_pq_hook(1)` / `mfree(Q)` after it (they moved from
  after today's single low product), then today's corrections and deferral.

**Model** (`mn_model.py`: `division_cost_dkm` under `MN_MODEL_DKM=1`; `ovl_div`, the early writer's overlap, at size > 1). The
command is `python3 ../tests/dkm15_model.py` in `ecalc/`, and the output is `archive:results_raw_2026-10-19/DKM15/model.txt`. All figures are modelled at
5.1 × 10¹³ on 576 nodes, with the fabric assumed, the write at 0.6 GB/s, and `ECALC_NP=auto`, on int15g:

| cache slots | today: no write / write | NEWTON_DKM=1 | gain | + an X_hi writer (not built) | gain |
|---|---|---|---|---|---|
| 2 | 330.7 / 360.2 s | 307.6 / 350.7 s | **−23.1 / −9.5 s** | 307.6 / 339.3 s | −23.1 / −20.9 s |
| 0 (what fits) | 406.8 / 426.0 s | 376.4 / 412.7 s | **−30.4 / −13.3 s** | 376.4 / 398.7 s | −30.4 / −27.3 s |

- The reciprocal goes 45.3 → 18.5 s and the division 53.3 → 57.0 s (cache 2). The pieces are recip 74 → 54 and div 40 → 42.
- DKM's four products are X_hi 9.4 s, X_hi·Q 17.1, X_lo 9.4 and X_lo·Q 17.1, with 7 / 14 / 7 / 14 pieces.
- These agree with SC's −22.1 / −31.9 s. SC's base was older, 324.3 s.
- The with-write gain is smaller than SC's estimate. The writer hides only under X_lo·Q and the rest, 17.1–23.9 s, against today's
  0.577 × the division. It is assumed fully overlapped.
- **A follow-up would recover it:** a writer hook after step 1 that writes X_hi's digits, which are final there, under step 2
  (`MN_MODEL_DKM_HI=1` prices it). It needs `mn_out.c` / `ecalc.c`: part files written in two ranges.

**Memory** (`mem_model.dm_layout(…, dkm=True)` / `mem_per_node(opts dkm=True)`; the default is unchanged, so `--check-c` is not
affected). **This corrects §1.4 and SC's ≤ −20 GB.**
- With a `dm_layout` that follows DKM (not built: binsplit.c), the node falls 455.4 → 450.3 GB at the target (**−5.1 GB**,
  modelled). At 10¹¹ it falls 410.4 → 405.0 GB.
- The hole halves (19.98 → 9.99 GB per device) and v3 falls 56.5 → 46.5. But the dm need is then bound by the division's
  low-product set, Q + X + Aw + X_lo·Q + X_lo ≈ 5.1 n per device, which is the same as today's.
- The −20 GB bound needs that set smaller as well: M6's chunked division window.
- **With the code as it is, there is no memory change.** The arena is carved to today's layout; DKM's blocks are smaller and fit
  in it.

#### 4. Measurements (aac6)

##### 4.1 Stage A: `NEWTON_DKM=1` correctness

Job 21766 ran on s24-26, 18:29–18:46 EDT, on commit 3fc51d6. The command was `archive:drivers/ecalc/dkm15_batch.sh A 3fc51d6`, and the logs are in
aac6 `~/dkm15tmp/A/` and `~/ntt-DKM15/ecalc/results/mnaccept/21766/`. The first attempt, job 21760, was cancelled by the scheduler
after 50 s on s24-16 and ran nothing.

| test | result (measured) |
|---|---|
| `NEWTON_DEVICE=1 ./tests/t_newton 20` | **section 6: every case identical** (X exact, R's residues). This covers 4 sizes × 7 shapes, off / on, forced ±7 on both hooks. The final count moved by exactly k under `ECALC_TEST_CORR`. Under `NEWTON_DKM_TEST_HI` the final count stayed put and step 1 made the 7. The s = dl shape took the fresh reciprocal, as designed. **4 failures in section 5** (R4's "the high cut skipped nothing" at n_q 3·2²⁰, 2²²): that section needs `DIST_LOGN_TEST=20` (R415.md); stage B re-runs it so |
| e9 `NEWTON_DKM=1`, LIMB_BASE 10 / 2 | identical, VERIFY OK (`total` 10.81 / 22.78 s). **But at 10⁹ ecalc takes the host flow** (`newton_db_divmod`, below the device tier's 2³⁰), so DKM is not exercised at size 1 there. Stage B repeats these with `BS_MDEV_LOGL=24` |
| e9 forced (`ECALC_TEST_CORR` +7 / −7 / +7 with `ECALC_CORR_PATCH` 1 / 2 / 0; `NEWTON_DKM_TEST_HI=7`) | identical, VERIFY OK (host flow, as above) |
| c511 (511461828, +14) / c820 (820719000, −44), `ECALC_CORR_PATCH=1` | **VERIFY FAILED, digits differ.** Host flow, so not DKM: the §4.3 defect on main |
| **4 × 10¹⁰ `NEWTON_DKM=1`** | **identical** (digcmp, packed). DKM taken: `divmod(dev, DKM) 12.13 s: k 2222222224 = 1111111112 + 1111111112, h 1111111113 (kept); step 1 A mu 2.14, X_hi Q + corrections 3.48 (0), step 2 A mu + assembly 2.15, X_lo Q 3.64, corrections 0.45 (0)`. recip 3.94 s, dm 16.07 s, `total` 54.28 s |
| `NEWTON_DKM=1 ./mnaccept.sh 21766 --only mn` | **5 passed, 0 failed** (e8 sizes 2, 3, 4; e9 sizes 2, 4; DKM taken: `divmod(mn, DKM) … k 55555557 = 27777779 + 27777778`) |
| `NEWTON_DKM=1 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7 ./mnaccept.sh 21766 --only mn,recheck` | **7 passed, 0 failed** (at 10⁸ / 10⁹ the changed digits lie past d_out) |
| c108d (−39) at sizes 2 and 4, `ECALC_CORR_PATCH=0` (in place) | **identical**, all nodes VERIFY OK |
| `NEWTON_DKM_TEST_HI=−9` at sizes 2 and 4; +9 with `ECALC_TEST_CORR=−5` at size 3 | **identical**. Step 1 made the 9 corrections; the final count was 0 or 5 |
| c108u (+36) / c108d (−39) at sizes 2 and 4, `ECALC_CORR_PATCH=2` | **VERIFY FAILED on node 0, digits differ.** Same signature as c511: the patch on node 0's packed part file. See §4.3 |

**Reading.** In every run where the division took the DKM path (t_newton section 6, 4 × 10¹⁰, mn at sizes 2–4 including the forced
corrections), the digits are identical. The failures are all in `mn_out`'s tail patch on a packed file. They occur with
`ECALC_CORR_PATCH` ≥ 1 when the changed digits lie inside the file, and they appear in the host flow too, where DKM does not run.

##### 4.2 Stage B: the gates with the switch off, the size-1 device flow at 10⁹, the patch controls

Job 21768 ran on s24-26, 18:46–19:19 EDT, on commit 5eb35dc. The command was `archive:drivers/ecalc/dkm15_batch.sh B 5eb35dc`, and the logs are in
aac6 `~/dkm15tmp/B/` and `results/mnaccept/21768/`.

| test | result (measured) |
|---|---|
| `NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20` | **VERIFY OK (1187 checks)**: sections 5 and 6 on grids |
| `./mnaccept.sh 21768 --only unit,e9` (switch off) | **10 passed, 0 failed**: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 in both bases identical |
| 4 × 10¹⁰, switch off | **identical**. recip 9.23 s, dm 20.90 s, `total` 57.50 s (A's on run: recip 3.94, dm 16.07, `total` 54.28; one run each, same node) |
| e9 through the device flow (`BS_MDEV_LOGL=24`), off / on | **identical**, VERIFY OK. The on run took DKM: `divmod(dev, DKM) … k 55555557 = 27777779 + 27777778, h 27777779 (kept)` |
| the same, forced: `ECALC_TEST_CORR` +7 (`ECALC_CORR_PATCH=1`), −7 (default), +7 (`=0`); `NEWTON_DKM_TEST_HI=7` | **identical**, VERIFY OK. The corrections were 0/7, 7/0, 0/7 and 0/0 |
| c511 / c820 through the device flow with DKM, `ECALC_CORR_PATCH=1` | VERIFY FAILED: the §4.3 defect |

##### 4.3 A defect on main, independent of DKM: the tail patch on the packed output

Sent to the integrator on 2026-09-28. With `NEWTON_DKM` unset, on B2's code (job 21768):
- `./ecalc 511461828` with `ECALC_TEST_CORR=14`, under `ECALC_CORR_PATCH=1` and under the default (2): **VERIFY FAILED, digits
  differ**. With `ECALC_OUT_PACKED=0` the same run is **identical**.
- With `ECALC_TEST_CORR=1`, **a single correction**: **VERIFY FAILED**.
- `./ecalc 820719000` with −44: VERIFY FAILED.
- mnrun 2 at 108388422 (`POOL_LOG=27 ECALC_CORR_PATCH=2 ECALC_TEST_CORR=36`): node 0 VERIFY FAILED. The ASCII form is identical.

The log says "patch: … holds 227320472 bytes, the patch ends at 511461830", then "patch … FAILED" and
"digits == X mod q FAILED". `mn_out_tail_fix` / `patch_bytes` (`mn_out.c`, not my file) write ASCII digits at ASCII offsets. A packed
part holds 8 bytes per 18-digit limb, so the offset lies past the end of the file.
- **Why the regression missed it:** its forced runs change only digits past d_out.
- **When it strikes:** any run whose division corrects X, with the changed digits inside the file (d_out near d), fails its VERIFY
  under the defaults.
- **What it means for DKM:** it does not bear on DKM. The same runs pass with `ECALC_CORR_PATCH=0`.

##### 4.4 Stage C / D: 10¹¹ timing, off against on (s24-26)

Stage C was job 21773, 19:20–20:05 EDT, on commit 5eb35dc (`dkm15_batch.sh C`). Each run wrote the packed file, was compared with
`digcmp.sh` against `results/e_1e11.out`, and had the reference evicted before it.
- **The compare against the cold reference on the shared file system takes ≈ 15 min** (off_1 ended 19:23, its compare 19:38). So
  the job reached only off_1, on_1 and off_2 (the last without a compare) before its limit.
- Stage D (job 21778, from 20:06 EDT, commit a55a14c) continues. Each file is `cmp`'d against the first run's, which is valid
  because the packed header has no run-dependent field, and the first file against the reference at the end.

| run | switch | wall with the file | `total` | recip | dm | division (verbose) | digits |
|---|---|---|---|---|---|---|---|
| off_1 | off | 183.7 s | 181.57 s | 36.46 | 88.70 | 52.24 s: A mu 24.87, X out + low product 25.74 | identical |
| on_1 | `NEWTON_DKM=1` | **162.7 s** | **154.08 s** | **11.76** | **63.07** | 51.31 s: step 1 A mu 7.15, X_hi Q 17.20, step 2 A mu + assembly 7.38, X_lo Q 18.17, corrections 0.74 | identical |
| off_2 | off | 183.7 s | 181.54 s | 36.48 | 88.82 | 52.34 s | VERIFY OK (not compared: the limit) |

Stage D ran as job 21778, 20:06–20:35 EDT, on commit a55a14c:

| run | switch | wall with the file | `total` | recip | dm | init / bs | digits |
|---|---|---|---|---|---|---|---|
| on_2 | on | 156.3 | 154.01 | 11.92 | 62.93 | 18.6 / 72.0 | **identical** (digcmp against `e_1e11.out`, 20:35 EDT) |
| off_2′ | off | 184.2 | 181.96 | 36.56 | 88.80 | 19.6 / 73.2 | the same bytes as on_2's |
| on_3 | on | 191.7 | 188.74 | 11.87 | 63.32 | **31.1 / 93.9** | the same bytes as on_2's |
| off_3 | off | 188.0 | 184.70 | 36.64 | 88.83 | — | the same bytes as on_2's |

**Summary (measured, s24-26; 4 off runs, 3 on runs):**
- **dm: 88.79 → 63.11 s (−25.7 s, −28.9 %).** The spread is ≤ 0.4 s per side.
  - The reciprocal accounts for **−24.7 s** (36.54 → 11.85).
  - The division itself goes 52.25 → 51.26 s (−1.0). The model's 1.0 → 1.0 n² area holds.
- **`total`: 182.4 → 154.0 s (−28.4 s), and the wall with the file: 184.9 → 159.5 s (−25.4 s)**, both over on_1 / on_2 against the
  four off runs.
  - on_3 is excluded as an outlier in phases that end before the division: init 31.1 against 18.6–19.6 s, and bs 93.9 against
    72.0–73.2 s. Its dm was 63.3 s.
  - Including on_3, the means are 165.6 / 170.2 s (−16.8 / −14.7 s).
- **The writer is less hidden, as modelled.** The file's exposed tail (wall − `total`) is 2.1–3.3 s off against 2.3–8.6 s on. The
  hook now comes before only step 2's low product, 18.2 s, against today's 25.7 s low product.
- **The digits are identical in every run** (§4.1–§4.4).

#### 5. Summary, open issues, what is outside my files

**Result.** `NEWTON_DKM=1` gives identical digits at every size tested:
- t_newton section 6;
- e9 through the device flow (off, on, forced);
- 4 × 10¹⁰;
- 10¹¹ (7 runs);
- mn e8 at sizes 2, 3, 4 and e9 at sizes 2, 4, including forced corrections (`ECALC_TEST_CORR`, `NEWTON_DKM_TEST_HI`) under
  `ECALC_CORR_PATCH` 0 and 2.

**Size 1** (measured, 10¹¹, one node): dm −25.7 s (−29 %), `total` −28.4 s, and the wall with the file −25.4 s.

**Size > 1** (modelled, 5.1 × 10¹³ on 576 nodes): −23.1 / −9.5 s at cache 2, and −30.4 / −13.3 s at cache 0 (no write / with the
write). With an X_hi writer (not built), the with-write figures would be −20.9 / −27.3 s.

**Memory:** no change as built, and −5.1 GB per node modelled with a `dm_layout` that follows it (not −20 GB: §3).

**Open issues:**
1. **The §4.3 defect on main (not DKM):** `mn_out_tail_fix` patches ASCII offsets into packed files. It has been reported to the
   integrator. Any correction whose digits lie in the file fails VERIFY under the defaults. The long-chain cases here pass only with
   `ECALC_CORR_PATCH=0` or `ECALC_OUT_PACKED=0`, and I have not re-run them with DKM after a fix.
2. **The plan printer does not follow the switch.** `mn_plan.c` is P24's file, so `MN_PLAN_ONLY` under `NEWTON_DKM=1` prints
   today's reciprocal and division pieces. It needs one `if` around `plan_recip_mn` / `show_mn` with the lengths of §1.2. The
   model's plan does follow the switch.
3. **`dm_layout`** (binsplit.c) does not know DKM. The −5.1 GB needs the hole sized to k + 8 and the reciprocal's jl at h.
4. **The X_hi writer** (§1.3), in `mn_out.c` / `ecalc.c`, would recover about half of the with-write gain at the target.
5. **The host flow** (`newton_db_divmod`, below the device tier, and binary limbs) has no DKM form. It is not a production path.
6. **Size > 1 was tested only on one node** (loopback, sizes 2–4, 10⁸–10⁹). The fabric terms of the model are assumed.
7. **One 10¹¹ on run (on_3) was an outlier in init / bs.** A clean paired series of ≥ 3 on runs without outliers would firm up the
   `total` figure. The dm figure is firm.

**Outside my files:**
- `archive:drivers/ecalc/dkm15_batch.sh` and `tests/dkm15_model.py` are new files.
- The README row.
- Nothing in `mn_out.c`, `binsplit.c`, `mn_plan.c`, `rns_dist.c` or `ecalc.c` was changed.

#### RESUME

**The work is complete** (stages 1–3 done). The branch is `p15-DKM`, and aac6 `~/ntt-DKM15` is at a55a14c. No jobs of mine are
running or pending, and the logs are in aac6 `~/dkm15tmp/{A,B,C,D}/`.

What remains is for the integrator or a successor:
- **Before any adoption:** the §4.3 fix on main (not DKM's), then DKM's long-chain cases re-run with the patch at sizes 1, 2 and 4.
- **Then:** the follow-ups in §5 (2–4), and the user's decision on `NEWTON_DKM`.

## EW15: the writer starts on X_hi's digits after DKM's step 1 (Phase 15 Batch 3, agent EW)

(former file: EW15.md)

Branch `p15-EW` from `int15i` ab8daa2 (main + P24 + MPB + DKM + MS's fixes; defaults `MN_P24=2`, `NEWTON_DKM=1`). The item is
results/DIVISION.md §DKM15 §1.3's follow-up, approved by the user on 2026-09-29. The aac6 clone is `~/ntt-EW15`. Times are Eastern. Every
number is labelled **measured**, **modelled** or **assumed**.

The switch is `MN_OUT_DKM_HI=1`, off by default. With it off, the code paths are the ones on the base.

#### 1. Design note (stage 1, written 2026-09-29 00:40–01:40 EDT)

##### 1.1 Today (ab8daa2, `NEWTON_DKM=1`)

The division makes the quotient in two halves (results/DIVISION.md §DKM15 §1.2). With s = min(⌊k/2⌋, dl):

1. Step 1 computes X_hi = ⌊A′/Q⌋ and R₁ exactly (its own corrections, never seen by anyone).
2. Step 2 computes X_lo₀ = ((R₁ >> (n_q − 1 − s))·μ₂) >> (k₂ + 1), the estimate of X_lo = ⌊R₁·B^s / Q⌋.
3. The assembly forms X₀ = X_hi·B^s + X_lo₀ (a new number: size 1 `db_add_shifted`, size > 1 three `mdb_shift` and one
   `mdb_addsub`). X_hi and X_lo₀ are freed (X_lo₀ is read as X₀'s low s limbs by the low product).
4. **The writer's hook** (size 1 `newton_db_x_hook` / `newton_db_x_dev`, size > 1 `newton_mn_x_hook` = `MN_OUT_EARLY`) starts the
   writer on X₀.
5. The low product X_lo₀·Q mod B^w, the window, the corrections dx (deferred to the output's tail patch under
   `ECALC_CORR_PATCH=2`; in place otherwise).

The writer therefore overlaps only step 2's low product and the corrections. Step 2's high product, its window and the
assembly (7.4 s at 10¹¹ on one node, measured in DKM15 §4.4; ≈ 7 s at the target, modelled) run with the writer idle.

##### 1.2 What can change after step 1: the proof

Let X = ⌊A/Q⌋ be the exact quotient and X_lo = ⌊A₂/Q⌋ with A₂ = R₁·B^s.

1. **X_hi is final after step 1.** Step 1's loop stops only at 0 ≤ R₁ < Q, so A₂ = R₁·B^s < Q·B^s and X_lo < B^s. Since
   A = A′·B^s = X_hi·Q·B^s + A₂, X = X_hi·B^s + X_lo with 0 ≤ X_lo < B^s. So X's limbs at and above s are exactly X_hi's limbs,
   and X's limbs below s are exactly X_lo's. **Nothing after step 1 changes X_hi's limbs**, whatever step 2 does.
2. **Step 2's corrections change only X_lo.** Step 2's loop makes X_lo₀ + dx = X_lo exactly (its stop condition is 0 ≤ R < Q).
   So the final low half is X_lo₀ + dx, a number in [0, B^s).
3. **The carry case.** X_lo₀ can exceed the truth by a few units (the estimate's error, and `ECALC_TEST_CORR=k` with k < 0 adds
   |k|). So X_lo₀ ≥ B^s is possible when X_lo ≥ B^s − 64, i.e. when X's low half is 99…9 followed by at most two other digits.
   Today that case is a carry from X_lo₀ into X_hi's part of X₀, taken back by the corrections (dx < 0).
   - With the digits of X_hi written from X_hi itself, X₀ is never formed, so there is no carry into the written half.
   - The low half written from X_lo₀ and patched by dx would be wrong: X_lo₀ has s + 1 limbs and the patch would have to turn
     its low s limbs (≈ 0…0xx) into X_lo (≈ 9…9xx): **every byte of the low half changes**. So in this case the corrections are
     applied to X_lo₀ in place first (no deferral), and the low half is written once from X_lo. That is the patch at its
     minimum cost; its probability at real sizes is ≈ 64 / B^s (never, at s ≥ 10⁶ limbs).
   - Without a carry (X_lo₀ < B^s), X_lo₀ + dx ∈ [0, B^s): **the deferred patch's carry / borrow stops inside the low half**,
     and today's tail patch (`mn_out_tail_fix`) applies unchanged.
4. **So what can change after the writer starts on X_hi:** nothing in the first ≈ half of the file (X_hi's digits), and in the
   second half only the tail the deferred correction's carry reaches (as today), or, in the carry case, nothing either (the low
   half is written after the correction).

##### 1.3 The design

**No assembly under the switch.** X₀ is never formed. The division hands X_hi and X_lo₀ to the output stage, which writes the
digits of X_hi as the file's high range and those of X_lo as its low range. This is memory-neutral or better:
- step 2's high product holds X_hi (n/2), as today;
- the low product holds X_hi + X_lo₀ (n) instead of X₀ (n), the same;
- the assembly's moment X_hi + X_lo₀ + X₀ (2n) disappears.

So the hook needs no copy of X_hi and no delayed free: X_hi simply lives until the output stage, in place of X₀.

**The hooks** (`newton_db.c`, the only lines outside my files besides the README):
- `newton_db_xhi_hook(dbig *Xh, size_t s, void *arg)` / `newton_mn_xhi_hook(mdb *Xh, size_t s, void *arg)` right after step 1's
  corrections (the call site `MN_OUT_DKM_HI: the X_hi hook` in `divmod_shifted_dkm` and `mn_divmod_dkm`). The hook takes
  ownership of X_hi and starts the writer on it.
- `newton_db_xlo_hook(dbig *Xl, size_t s, int carry, void *arg)` / `newton_mn_xlo_hook(mdb *Xl, …)` where today's hook is (after
  step 2's estimate and `ECALC_TEST_CORR`, before the low product). carry = X_lo₀ ≥ B^s (`Xl.n > s`, known on every rank). It
  returns whether the writer was released onto X_lo₀. It releases it unless carry is set (or the test knob below).
- After the corrections: if the hook released the writer, dx is deferred (`newton_x_dx`, today's patch); if not, dx is added to
  X_lo₀ in place (`dkm_add_small` / `mn_dkm_add`), so X_lo₀ becomes X_lo and `newton_x_dx = 0`. The output stage then releases
  the writer. The division returns X = X_lo (size 1 in `newton_db_x_dev`, size > 1 in `*X`).
- With the switch on, the DKM path takes the new hooks only when the corrections are deferred (`newton_x_defer`: the default
  `ECALC_CORR_PATCH=2`, or 1 at size 1), the size-1 device flow (`newton_db_x_dev`) or `MN_OUT_EARLY=1` at size > 1, and decimal
  limbs. Otherwise it says so once and the division runs as today. The non-DKM division is unchanged.

**Size 1: one file in two ranges** (`mn_out.c`). `mn_out` gets `split` and `src_hi`: limbs ≥ split are read from `src_hi` (X_hi,
global limb s + i), limbs < split from the main source (X_lo). The writer runs top-down as today, with no chunk crossing split,
and calls `at_split` once when it reaches split (a gate). ecalc's callback:
1. posts "at the gate";
2. waits for the release (the X_lo hook, or the output stage in the carry case);
3. computes X_lo's residues (X_hi's were taken at the start), so X mod q = X_hi·B^s + X_lo;
4. posts `res_ready`.
The same writer, file, O_DIRECT carry and T2 head continue below split. So the bytes are the ones a one-range writer writes; only
the chunk boundaries move.

**Size > 1: each rank writes two part files.** X_hi and X_lo₀ are sharded evenly over the ranks (their own bases), but X₀'s
shares are contiguous. So with today's part layout (one file per rank's share of X₀), the ranks holding X's low half would have
nothing to write before the release, and they would bound the job's wall with no gain. So under the switch:
- rank r writes **part size − 1 − r** = its share of X_hi (global limbs s + [N_h r/g, N_h (r+1)/g)); the top rank's covers the
  leading limbs up to nl;
- rank r writes **part 2·size − 1 − r** = its share of X_lo (clipped to limbs below s; the top rank's extends to s);
- nparts = 2·size.

So `cat <outfile>.part*` (sorted) is still the file, and `tools/unpack_digits` takes the parts as they are (consecutive part
indices, headers with lo / hi / k0 / k1).
- **The T2 heads:** the high layer's are all-gathered at the X_hi hook (collective). The low layer's are gathered when the writer
  is released; the top low part's head is the high layer's last 49 digits.
- **The digit residues** join the 2·size parts in part order.
- **X mod q** is the sum of both layers' shares, each at its offset.
- **The patch** runs on the low parts. Its low-limb gather also reads the high shares, in case the context crosses s (tiny sizes
  only).
- **`ECALC_RECHECK`** reads parts j·size + size − 1 − r, j = 0, 1, when they exist (the same layout detection at every rank).

**A test knob** `MN_OUT_DKM_HI_TEST_CARRY=1` makes the X_lo hook refuse the release as in the carry case. The output-side path of
that case (in-place correction, the release after the division, patch with dx = 0) then runs at real sizes. The carry itself cannot
occur for e's digits (it needs ≈ 18·s nines), so the arithmetic case is tested in t_newton with constructed operands: X's low half
= B^s − 3 and `ECALC_TEST_CORR=−7` → X_lo₀ ≥ B^s.

##### 1.4 What composes with what

| with | how |
|---|---|
| packed output (default) | unchanged per part; size 1 one file; size > 1 two parts per rank (the header's part / nparts) |
| `ECALC_CORR_PATCH=2` | required. No carry: today's tail patch on the low range. Carry: dx in place before the low range is written, patch with dx = 0 (the deferred windows only) |
| `ECALC_CORR_PATCH=0` | the switch is off (a message): the in-place corrections would race the low-range writer; today's path (redo) is kept |
| the in-run checks | digits == X mod q with X mod q = X_hi·B^s + X_lo (+ dx) and the digits' residues joined over the ranges; T2 across the split by the carried or gathered heads |
| `MN_OUT_EARLY=1` (default) | required at size > 1 (it is the early writer, started one hook earlier) |
| RECHECK | the same file at size 1; 2·size parts at size > 1 (the recheck reads both layers) |
| `NEWTON_DKM_TEST_HI`, `ECALC_TEST_CORR` | unchanged: TEST_HI acts before step 1's corrections (the X_hi hook comes after them); TEST_CORR on X_lo₀ |

##### 1.5 Worth it (modelled at the user's new target, 5.167 × 10¹³ on 576, write 1.0 GB/s per node, cache 0, the base's defaults)

`estimate.py --g 576 --D 8.9705e10 --cache-slots 0` on ab8daa2 (modelled): 286.3 s without the write, **307.0 s with it**. The part
file is 39.9 GB per node (exact), 39.9 s at 1.0 GB/s (assumed rate).
- The writer starts step 2's high product, its window and the assembly earlier: ≈ 7 s (modelled).
- While the high range takes longer to write than that (19.9 s ≥ 7 s here), **the gain is that whole interval**: 307.0 → 299.8 s
  (−7.2 s, modelled with the model's existing `MN_MODEL_DKM_HI` term, which assumes the writer is never idle; stage D refits it as
  a pipeline).
- At 0.6 GB/s it is the same −7.2 s (333.6 → 326.4).
- DKM15's −20.9 / −27.3 s were DKM plus this writer against the pre-DKM division at 0.6 GB/s. The writer's own share is smaller
  now, because P24 shrank step 2.
- At size 1 (10¹¹ on aac6) the exposed tail is 2.3–8.6 s (measured, DKM15 §4.4). The high range written under step 2's 7.4 s
  should hide most of it.

**Verdict: safe** (§1.2: the written high range can never change; the low range changes only by today's tail patch, or is written
after the correction). **Worth it:** ≈ −7 s at the target with the file (modelled), at no memory cost (§1.3). Proceeding.

#### 2. The code (stages 2 and 3: size 1 and size > 1 share the hooks)

**`newton_db.c`** (DKM's file; the lines outside my own files, all under the new hooks):
- **The X_hi hook sites**, right after step 1's corrections: `divmod_shifted_dkm` (size 1) and `mn_divmod_dkm` (size > 1). They
  are named `MN_OUT_DKM_HI: the X_hi hook` in the comments. The hook returns whether it took X_hi. At size > 1 it declines when
  s or X_hi.n < 4·size, so that no layer's part can be empty; the division then runs as without the switch.
- **The X_lo hook branches**, where today's hook is (after step 2's estimate and `ECALC_TEST_CORR`):
  - carry = X_lo₀ ≥ B^s;
  - the low product runs on X_lo₀, and there is no X₀;
  - the corrections are deferred if the hook released the writer, and otherwise added to X_lo₀ in place;
  - the division returns X_lo (`newton_db_x_dev` / `*X`);
  - the verbose line reads `divmod(dev|mn, DKM, X_hi writer)`, with the corrections' mode and X_lo₀ < / ≥ B^s.
- **`dkm_add_small`**: a positive add may grow a limb (`db_add`). Before, `db_add_small` stopped on a carry out of the top limb.
  This only matters for the test hook `ECALC_TEST_CORR` < 0 on an X_lo₀ near B^s, which t_newton §7 constructs. The values are
  unchanged.
- The declarations are in `newton.h`.

**`mn_out.c` / `mn_out.h`:**
- `mn_out.split` / `src_hi` / `at_split`: one file in two ranges (size 1). The limbs ≥ split come from `src_hi`, no chunk crosses
  split, and the gate is called once, after every chunk above split has been handed to the writer thread. The same writer, file,
  O_DIRECT carry and T2 head continue below split.
- `lim`: X_lo's layer ends at s. The top rank's part reaches s, not nl.
- `part` / `nparts`: the explicit part index. It is used in the file name, the packed header, the stripe and the patch.
- `mn_out_early_hi_start` / `mn_out_early_lo_release` / `mn_out_early_hi`: the size > 1 early writer in two layers.
  - The X_hi part is started at the X_hi hook (a collective: the high layer's T2 heads, and X_hi's last 49 digits kept), and it is
    finished in the thread.
  - The X_lo part starts at the release (a collective: the low layer's heads, the top one after X_hi's last digits).
- `mn_out_digit_res_layers`: the digit residues over the layers.
- `mn_out_tail_fix`'s gather also reads `src_hi`, in case the context crosses s.
- The recheck reads part j·size + size − 1 − r for j < J, with J = 2 when part 2·size − 1 − r exists on some rank. This also fixes
  a latent 1-byte overflow of the recheck's tail slot (49 tail chars into 48 bytes); each slot is now 64 bytes.

**`ecalc.c`:**
- Size 1:
  - `x_hi_hook` takes X_hi and starts the writer thread;
  - the X_hi residues are taken first, the X_lo residues at the gate, and T1's X mod q = X_hi·B^s + X_lo;
  - `x_lo_hook` releases the writer unless carry (or `MN_OUT_DKM_HI_TEST_CARRY=1`);
  - main releases it after the division otherwise.
- Size > 1: `mn_early_hi_hook` / `mn_early_lo_hook`, with the release after the division in the carry case.
- `out_stage`:
  - X mod q from both layers;
  - the digit residues joined over both layers;
  - both parts' T2 windows counted;
  - both files printed;
  - X_hi freed at the end.
- The switch is set only where it can act: size 1 with the device flow and `ECALC_CORR_PATCH` ≥ 1; size > 1 with `MN_OUT_EARLY=1`,
  an output file and `ECALC_CORR_PATCH=2`. Otherwise it prints `MN_OUT_DKM_HI ignored`.

#### 3. Correctness (aac6; every result measured)

##### 3.1 The unit test on the login node

`tests/t_patch` (both forms, run by itself) passed on 2026-09-29 at 01:40 EDT with its new EW section, 40 random X of 1000–7000
digits. Each is written as X_hi·B^s + X_lo₀ with a correction dx ∈ ±[1, 9] inside X_lo, and carry chains of up to s − 2 whole limbs.
- **Size 1:** the split writer.
  - The gate is called exactly once, after the chunks above s (80 gates).
  - X_lo's source holds garbage until the gate fills it.
  - The file is patched through `mn_out_tail_fix` (its gather reads `src_hi`).
  - The carry path: the gate gives the corrected X_lo, and dx = 0.
- **Sizes 2–5:** the two layers.
  - X_hi's shares are in basis n_h + 1, X_lo₀'s in basis s + 2 below lim = s.
  - The low parts are patched.
- **Checks:**
  - the files equal the corrected file (ASCII `cmp`; packed: each part's limbs and header residues, and `tools/unpack_digits --cmp`);
  - digit residues + the adjustment = the corrected string's;
  - every window, including three across the split, is checked exactly once;
  - first / last / tail.
- **Results:** `ECALC_OUT_PACKED=0` 7765 checks and `=1` 8965 checks, **0 failures**, 1200 parts in two ranges or layers.

##### 3.2 Stage A1: the regression with the switch (job 21803, s24-26, 01:43–02:13 EDT, 7a1e4ff)

**`MN_OUT_DKM_HI=1 ./mnaccept.sh 21803 --only unit,e9,mn,recheck,corr`: 21 passed, 0 failed** (1557 s; logs
`~/ntt-EW15/ecalc/results/mnaccept/21803_a/`).
- The X_hi writer was taken (its log line) in mn e8 at sizes 2, 3 and 4, mn e9 at sizes 2 and 4, recheck e8 at size 2, and corr
  c108 at sizes 2 and 4. The recheck at size 2 read "…part0000 **and its X_lo part**", and a flipped digit failed it.
- At size 1 the 10⁹ runs take the host flow (below the device tier; DKM15 §4.1), so the switch does not act there.

**`MN_OUT_DKM_HI=1 BS_MDEV_LOGL=24 ./mnaccept.sh 21803 --only e9,recheck,corr`: 7 passed, 0 failed** (114 s). The size-1 X_hi writer
was taken in e9 (decimal), recheck e9 (RECHECK OK on the split file; the corrupted copy FAILED) and **corr c511** (+14: the patch
rewrote 8 bytes inside the split file; VERIFY OK; RECHECK OK on the patched file).

`NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20` **failed in the new §7** at the constructed carry case.
- The test hook `ECALC_TEST_CORR=−20` could not push X_lo₀ past B^s, because `dkm_add_small` stopped on a carry out of X_lo₀'s top
  limb ("newton_db: X carries out of its top limb"). The estimate itself can exceed B^s, and it then has s + 1 limbs; the hook could
  not.
- Fixed in 69ff0c1: a positive add now grows a limb. Every §7 case before that one passed: X_hi exact, X_lo₀ + dx = X mod B^s, R's
  residues.
- Re-run in stage A3 (§3.4).

##### 3.3 Stage A2: forced corrections, the carry path, 4 × 10¹⁰ (job 21807, s24-30, 02:32–02:44 EDT, 7a1e4ff)

The logs are in `~/ew15tmp/A2/`. Every run: the X_hi writer taken, `identical` (digcmp against `ref/e_<d>.txt`, or the first d digits of
e_1e9 + a newline), VERIFY OK on every node, rc 0. The size-1 runs are at 10⁹ through the device flow (`BS_MDEV_LOGL=24`); sizes 2 and
4 are at 10⁸ (`POOL_LOG=27`).

| run | size | forced | step 1's corrections | step 2's (mode) | patch |
|---|---|---|---|---|---|
| s1_p7 / s1_m7 | 1 | `ECALC_TEST_CORR` +7 / −7 | 0 | +7 / −7 (deferred) | X ± 7 patched |
| s1_hi7 | 1 | `NEWTON_DKM_TEST_HI=7` | 7 | 0 | nothing |
| s1_hi9_p5 | 1 | `NEWTON_DKM_TEST_HI=−9`, `ECALC_TEST_CORR=5` | −9 | +5 (deferred) | patched |
| **s1_carry_m7** | 1 | `MN_OUT_DKM_HI_TEST_CARRY=1`, −7 | 0 | −7 (**in place**) | dx = 0 |
| **s1_carry_c511** | 1 (511461828) | carry path, +14 (digits inside the file) | 0 | +14 (**in place**) | dx = 0 |
| s1_c511 / s1_c820 | 1 | +14 / −44 (digits inside the file) | 0 | deferred | patched |
| c108u / c108d | 2, 4 | +36 / −39 (node 0's X_lo part) | 0 | deferred | patched |
| hi9 | 2, 4 | `NEWTON_DKM_TEST_HI=−9` | −9 | 0 | nothing |
| **carry_c108u** | 2, 4 | carry path, +36 | 0 | +36 (**in place**) | dx = 0 |
| e9_3 | 3 (10⁹) | `NEWTON_DKM_TEST_HI=9`, `ECALC_TEST_CORR=−5` | 9 | −5 (deferred) | patched |

**4 × 10¹⁰ with `MN_OUT_DKM_HI=1`: identical** to `results/e_4e10.out` (digcmp), VERIFY OK.
- `divmod(dev, DKM, X_hi writer) 14.28 s`: step 1 A mu 2.48, X_hi Q 4.20, step 2 A mu 2.76, X_lo Q 3.84, corrections 0.73.
- dm 18.74 s, `total` 58.36 s; the file 17.78 GB, written in 6.87 s in the writer thread, 0.00 s after the checks. One run.

##### 3.4 Stage A3 (job 21809, s24-26, 02:47–02:52 EDT, 4da5578)

- `NEWTON_DEVICE=1 DIST_LOGN_TEST=20 ./tests/t_newton 20`: **VERIFY OK (1218 checks)**.
  - §7 covers 3 sizes × 2 low halves × 5 forcings.
  - **The constructed carry X_lo₀ ≥ B^s arose 6 times.** Each time the hook refused, the corrections went in place, and dx = 0.
  - In every case X_hi was exact, X_lo was right and R's residues were right.
- ASCII output (`ECALC_OUT_PACKED=0`) with the switch: s1 +7, s1 c511 +14, c108u at size 2, and the carry path at size 4. **All identical,
  VERIFY OK.**
- The ignored paths print their message and run as today, identical:
  - `ECALC_CORR_PATCH=0` at size 1;
  - `ECALC_CORR_PATCH=0` at size 2;
  - `MN_OUT_EARLY=0` at size 2.
- **`ECALC_RECHECK` over two layers at size 3** (6 parts), packed and ASCII: 4 RECHECK OK. With a digit flipped in an X_hi part:
  4 RECHECK FAILED.

#### 4. 10¹¹ (stage C, job 21811, s24-26, 02:52–03:37 EDT; measured)

| run | wall with the file | `total` | dm | digits |
|---|---|---|---|---|
| on_1 | 170.0 s | 167.73 | 70.67 | **identical to `e_1e11.out`** (digcmp) |
| off_1 | 168.6 s | 166.38 | 71.04 | the same bytes as on_1 |
| on_2 | 167.2 s | 164.92 | 70.21 | not compared (the job's limit) |

The reference compare took ≈ 15 min, so the job hit its 45-min limit after on_2; the others did not run. The exposed write is ≈ 2.3 s
(wall − `total`) both on and off, so on aac6's local /tmp the file is already nearly hidden at 10¹¹, and the switch cannot show a
gain there.
- **The timing series (≥ 3 each) is not done** (open; stopped by the integrator's token budget).
- The size-1 fractions `OVL1_HI` stay at DKM15's measured split (7.38 / 18.9 of 51.3 s). They are not refitted.

#### 5. The model (modelled; `python3 ../tests/ew15_model.py` in ecalc/)

At 5.167 × 10¹³ on 576 nodes, cache 0, the base's defaults:

| write rate (assumed) | with the file, off → on | gain |
|---|---|---|
| **1.0 GB/s** | **306.9 → 299.7 s** | **−7.2 s** |
| 0.6 GB/s | 333.4 → 326.3 s | −7.1 s |
| 2.0 GB/s | 286.9 → 280.7 s | −6.2 s |

- Without the file, 286.2 s (unchanged).
- The pipeline's inputs:
  - W = 39.9 s of file at 1.0 GB/s;
  - A = 7.2 s (step 2's high product, before the release);
  - B = 13.7 s (after the release);
  - f_hi = 0.50.
- The gain is ≈ A whenever X_hi's half (19.9 s) outlasts A.
- Memory: no increase (X₀ is never formed; X_hi + X_lo = n limbs in place of X₀).

#### 6. Summary, open issues, outside my files

**Result:** `MN_OUT_DKM_HI=1` (off by default) gives identical digits and VERIFY / RECHECK OK in every run: mnaccept 21 + 7, A2's 16 forced
runs, A3, 4 × 10¹⁰, and 10¹¹. That covers sizes 1–4, both output forms, forced corrections in both halves, and the carry path.
Modelled: −7.2 s at the target at 1 GB/s.

**Open:**
1. The 10¹¹ timing series (≥ 3 on / off) and the size-1 refit. Run `ew15_batch.sh C` without the reference compare, or in two jobs.
2. The gain is visible only where the write is exposed (the target's Lustre); aac6's /tmp hides it already.
3. The two-layer part files (2·size parts) change the part count at size > 1. Check the target's scripts, which take the sorted
   `.part*` files.

**Outside my files:**
- `newton_db.c`: the X_hi / X_lo hook sites and the `dkm_add_small` limb growth;
- `newton.h`: the declarations;
- `mn_out.c`'s recheck: a latent 1-byte tail-slot overflow is fixed.

#### RESUME

- Stage 1 (the note): 9f480b2.
- Code (size 1 and size > 1 together; they share the hooks), committed 7a1e4ff and later:
  - `newton_db.c`: the hook sites;
  - `mn_out.c`: split + gate, `lim`, part / nparts, the two-layer early writer, `mn_out_digit_res_layers`, the patch's gather
    over `src_hi`, the recheck over two layers;
  - `ecalc.c`: the hooks and `out_stage`;
  - tests: t_patch EW section, t_newton §7;
  - `archive:drivers/ecalc/ew15_batch.sh`.
- The model: `mn_model.early_exposed` and `tests/ew15_model.py` (61efdb5).
- t_patch (both forms) passed on the aac6 login node, 01:40 EDT.
- **The work is complete** except the 10¹¹ timing series (§6, open 1). No jobs are running or pending. The last node commit is 4da5578; the
  report commits follow it. aac6 `~/ntt-EW15` is at 4da5578; the logs are in `~/ew15tmp/{A1,A2,A3,C}`.

## K15 — a correction without a rewrite (PLAN §36, row 1d)

(former file: K15.md)

Agent K, branch `p15-K` from int15 0b80ca3 (B0). aac6 clone `~/ntt-K15`. Times Eastern.

#### 1. What happens today (read from the code, base 0b80ca3)

- **The range of a correction.** The three division paths end in a correction loop that moves X by one per step and stops at
  64 steps (`ec_fatal` beyond): `newton_db_divmod` (the host flow, `bi_sub`/`bi_add_u64` of 1), `newton_db_divmod_shifted`
  (the device flow at size 1: `dx` summed, then `db_add_small(X, dx)`), `newton_mn_divmod` (size > 1: `mdb_add_val(Xn, 0, |dx|)`).
  So X_final = X_hook + dx with |dx| ≤ 64; only the digits the carry / borrow of dx reaches change.
- **Size 1 (the overlapped device flow, the default).** The hook `x_bg_hook` starts the writer thread on the device X *before
  the low product*; the corrections come after it and were applied to the device X **in place, while the writer thread may be
  reading it**. `out_stage` then (`ecalc.c`) (a) joins the writer in T1 (`xres_bg = … && !c->ncorr`: V2's 20.9 s at 10¹¹)
  and (b) re-runs the whole writer on the corrected X ("X corrected after the formatting started: redoing the digits"): the file
  is written twice (dc 57.0 s vs 22.5 s).
- **Size > 1 (the distributed division).** `newton_mn_divmod` applies the correction to the sharded X *before* `out_stage`
  starts any part file: **there is no rewrite at size > 1 today** (and so none at the 576-node target on the current code).
  The rewrite V2 measured is a size-1 cost only. The patch is still built for part files (switch value 2), so it is ready for
  a multi-node writer that starts before the corrections (as size 1's does), and it is tested across part boundaries.

#### 2. Design (`ECALC_CORR_PATCH`, off by default)

- `=1`: at size 1, when the hook has started the writer, the division does **not** apply its corrections to X: their sum comes
  back in `newton_x_dx` (`newton_db.c`: both size-1 paths). The writer reads an X that never changes under it (the in-place race
  of the off path is gone as well).
- **T1 never waits for the writer**: it takes X's residues from the writer thread as before (`res_ready`, before the formatting)
  and adds dx (the node holding limb 0). The writer finishes in the background; the join stays where it was (dc, after `total`).
- **The patch** (`mn_out_tail_fix`, after `mn_out_finish`): the low limbs of X are read (the device X, size 1; gathered over the
  nodes by an all-gather of the low window, size > 1), `dx` is added with its carry / borrow (more limbs gathered — ×4 — while it
  runs on), the old and new digits of the changed tail are formatted, and every node whose part file holds changed digits:
  reads the 4 KiB blocks holding them (O_DIRECT when the writer used it), **checks they hold the uncorrected digits**, writes the
  new ones, fsyncs, cuts the file back to size if the last block went past it, and **reads them back** and checks them.
- **The checks see the corrected digits**: the digit residues are adjusted by new − old of the changed tail (only the lowest
  digits change, so no shift), and the old / new tails were compared to the bytes on disk before and after the write — so
  `digits == X mod q` holds for the patched file. T2: the writer leaves the windows ending in the last `ECALC_CORR_PATCH_ZONE`
  (4096) digits; the patch checks them on the new digits (so no window is reported BAD on the uncorrected digits). A chain longer
  than the zone: the windows over it are re-checked and their old verdicts replaced. `last`, the sidecar's `tail` (digits after
  d_out), `first` follow; the sidecar gets the corrected residues (ECALC_RECHECK works on the patched file).
- `=2`: also at size > 1 (`newton_mn_divmod` defers dx; every node calls the collective patch; the carry may cross part files).
- **Test hook** `ECALC_TEST_CORR=<k>` (|k| ≤ 60): X − k right after X is formed (all three paths), before the hook: the loop
  then makes k up-corrections (k > 0) or |k| down-corrections (k < 0); the final X is e's, so every output compares with the
  reference, on the switch's path and on the rewrite path alike.

##### 2.1 Both write orders (the integrator's note from MD15, 2026-09-26)

The patch does not depend on when the part file was written. Its contract: **on each node, `mn_out_finish(o)` has returned
(the writer joined, the file complete) before `mn_out_tail_fix(o, …)`**, and the X the writer read is the uncorrected one
(the division deferred dx). Then:
- **file written after T1** (size > 1 today): `out_stage` runs `mn_out_run`, `mn_out_finish`, then the patch;
- **file still streaming when the corrections happen** (size 1 today; size > 1 once IO's W5 streams the part files with the
  low product): the division defers dx instead of changing X under the writer, T1 takes the residues + dx without joining,
  and the patch runs after the writer's own final join in dc (`out_stage` calls `mn_out_finish` first; a second call is a no-op).
- For IO's multi-node streaming (W5): when its writer starts before `newton_mn_divmod`'s corrections, it must either run with
  `ECALC_CORR_PATCH=2` (X is then not corrected in place: `mdb_add_val` is skipped) or join and rewrite as size 1's off path does
  — without either, the in-place `mdb_add_val` would race the writer's reads of the share (the same race size 1's off path had).
  The one coupling point: `newton_x_defer` must be set before the writer starts, and `o->t2_defer` set on the streaming writer's
  `mn_out` (as `x_bg_writer` does at size 1), so its T2 windows in the zone are left to the patch.

**Functions touched.** `mn_out.c`: `mn_out_run` (one argument of its `tier2_range` call), new `sub_mod`, `patch_bytes`,
`mn_out_tail_core`, `mn_out_tail_fix`. `mn_out.h`: struct `mn_out` (+`t2_defer`), new `mn_out_fix`, the two prototypes.
`ecalc.c`: `x_bg_writer`, `out_stage`, `main` (struct `x_bg`, `out_ctx` fields). `newton_db.c`: `newton_db_divmod`,
`newton_db_divmod_shifted`, `newton_mn_divmod`, new `newton_test_corr`. Nothing of the writer thread (`writer_run`,
`direct_job`, `mn_out_finish`) or of the recheck is touched.

#### 3. Lines touched (for the merge with IO's `mn_out.c`)

Base 0b80ca3 line numbers; every added line carries a "Phase 15 K" comment.

- `ecalc/mn_out.c`: **+1 line** after 15 (`#include <sys/stat.h>`); **1 changed line**, 258 in `mn_out_run` (the `tier2_range` call's
  `ndig` argument: `o->d_out + 1 - min(o->t2_defer, o->d_out + 1)`; with `t2_defer = 0` it is the old value); **+115 lines** inserted
  after `mn_out_digit_res` (after 308), a self-contained block: `sub_mod`, `patch_bytes`, `mn_out_tail_core`, `mn_out_tail_fix`.
  Nothing else in the writer, `mn_out_finish`, or the recheck is touched. It relies on `fmt_limbs`, `dev_fetch`,
  `mn_out_allgather_u64`, `sp_odirect`/`SP_ALIGN` and the `ECALC_OUT_ODIRECT` rule of `mn_out_run` (a patched block is opened with
  O_DIRECT exactly when the writer's file was); if IO changes the file layout (packed output `ECALC_OUT_PACKED`), the patch must
  follow it: under a packed format the tail is the low limbs themselves (8 bytes per 18 digits), and the core's byte image
  (`pos(k) = (k ? k + 1 : 0) - fbase`) is the one place to change.
- `ecalc/mn_out.h`: +1 field `size_t t2_defer` at the end of `mn_out` (after `priv`); +13 lines (`mn_out_fix`, `mn_out_tail_fix`,
  `mn_out_tail_core`) before `mn_out_digit_res`'s declaration.
- `ecalc/ecalc.c` (the correction path): `struct x_bg` + `t2_defer` (78); `x_bg_writer` sets it (83); `out_ctx` + `defer, dx, zone`
  (after 110); `out_stage`: `xres_bg` also when deferred (150), X's residues + dx (after 153), the redo only when not deferred
  (184), `o->t2_defer` for the size > 1 writer (after 189), the patch call and its two lines (after 192), `Dres` + the
  adjustment (after 193), `bad3 |= badp` (196); `main`: the switch (after 407), `oc.defer/dx` after `newton_mn_divmod` (after 467)
  and at size 1 (after 665).
- `ecalc/newton.h` +6, `ecalc/newton_db.c` +20 / 4 changed: `newton_x_defer`, `newton_x_dx`, `newton_test_corr` (the hook), the
  deferral in `newton_db_divmod`, `newton_db_divmod_shifted`, `newton_mn_divmod`.
- Outside the owned files (minimal): `ecalc/Makefile` (`tests/t_patch` in TESTS and its rule); new `ecalc/tests/t_patch.c`,
  `archive:drivers/ecalc/k15_batch.sh` (the node batches).

#### 4. Tests

All measured, commit a08c271 in `~/ntt-K15` (the code is that of 1580160; later commits are docs).

**The unit test `tests/t_patch`** (new): 80 random X of up to 6000 digits in base 10¹⁸, dx ∈ [−9, 9] \ {0} with a forced
carry / borrow chain of 0 .. nl − 3 whole limbs of 9s / 0s plus a run of 9s / 0s inside the limb where it ends (and 1 in 7 without
any carry out of limb 0), split over 1..5 "nodes" (part files) and chunks of 1..9 limbs, T2 zones 0 / 30 / 100 / 4096; the
parts written from the uncorrected X, then patched node by node (size 1 through `mn_out_tail_fix`, else the core over growing
windows as the collective does); against the file written from the corrected X (the rewrite path's output).

| where | command | result |
|---|---|---|
| login node, `/shared` (NFS) | `T_PATCH_DIR=~/k15tmp ./tests/t_patch`, ECALC_ODIRECT=1 and 0 | 5165 checks, 0 failures (longest chain 5976 digits, 69 chains over one limb); the O_DIRECT patch path taken (716 `O_RDWR|O_DIRECT` opens with =1, 0 with =0: strace) |
| s24-30, job 21468, node `/tmp` | the same, ECALC_ODIRECT=1 and 0 | 5165 checks, 0 failures, both |

**Batch A — the gates with the switch off** (s24-30, job 21468, 2026-09-26 23:38–00:06 EDT): `t_newton 20` VERIFY OK (620
checks), `t_mul 20` VERIFY OK (189); `./mnaccept.sh 21468 --only unit,e9,mn`: **15 passed, 0 failed** (1296 s): unit t_ntt 24,
t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 size 1 LIMB_BASE=10 identical (total 11.55 s),
LIMB_BASE=2 identical (20.48 s); mn e8 sizes 2, 3, 4 identical; mn e9 sizes 2, 4 identical.

**Batch B — forced corrections** (s24-30, job 21477, 2026-09-27 01:36–01:42 EDT; every output compared with the first d_out
digits of `ref/e_1000000000.txt`, read with O_DIRECT).

`ECALC_CORR_PATCH=2 ECALC_TEST_CORR=7 ./mnaccept.sh 21477 --only e9,mn,recheck`: **9 passed, 0 failed** (160 s). Every run made
7 forced up-corrections, deferred them, and patched the files. Results: e9 size 1 in both bases identical; mn e8 sizes 2, 3, 4 and
e9 sizes 2, 4 identical with all nodes VERIFY OK; recheck e9 size 1 and e8 size 2: RECHECK OK on the patched files (the sidecar's
corrected residues and tail), and RECHECK FAILED on the corrupted copy, as required.

The longest chains in e's first 10⁹ digits, for sizes d ≡ 0 (mod 18) and |k| ≤ 60 (search: `chain.py` over the reference).
X ends in a run of 0s or 9s, so X − k borrows / carries through it. `on` = `ECALC_CORR_PATCH=1` (size 1) or `=2` (size 2, 4);
`off` = the rewrite path at size 1 and the in-place correction at sizes 2 and 4:

| run | d_out | k | corrections | digits changed | patch | result |
|---|---|---|---|---|---|---|
| e9 on | 10⁹ | −7 | 7 down | [1000000007, 1000000008] (past d_out: 0 bytes; the sidecar tail, the residues and the deferred @10⁹ window follow) | 0.001 s | identical, VERIFY OK |
| e9 off | 10⁹ | +7 | 7 up | whole file redone | — | identical, VERIFY OK |
| c511 on | 511461828 | +14 | 14 up | [511461818, 511461828]: 11 digits (a run of 8 zeros) | 11 bytes, 0.007 s | identical, VERIFY OK |
| c511 on, d_out = d − 5 | 511461823 | +14 | 14 up | the same; 6 bytes in the file (O_DIRECT, one 4 KiB block), the rest in the tail | 0.007 s | identical, VERIFY OK |
| c511 off | 511461828 | +14 | 14 up | redone | — | identical |
| c820 on | 820719000 | −44 | 44 down | [820718991, 820719000]: 10 digits (a run of 9s) | 10 bytes, 0.007 s | identical, VERIFY OK |
| c820 off | 820719000 | −44 | 44 down | redone | — | identical |
| c108u on, sizes 2 / 4 | 108388422 | +36 | 36 up | [108388413, 108388422]: node 0's part, 10 bytes | 0.008 / 0.017 s | identical, all nodes VERIFY OK |
| c108d on, sizes 2 / 4 | 108072396 | −39 | 39 down | [108072388, 108072396]: node 0's part, 9 bytes | 0.009 / 0.023 s | identical, all nodes VERIFY OK |
| c108d off, sizes 2 / 4 | 108072396 | −39 | 39 down | in place before the output | — | identical, all nodes VERIFY OK |

At these sizes e's digits give chains of at most 11 digits, inside one limb. Chains across limbs, chunks and part files
(up to 5976 digits, 69 cases over one limb, 1–5 parts) are covered by `t_patch` only. A 10⁹-digit prefix of e has no run of 18
nines or zeros at a d ≡ 0 (mod 18) end.

#### 5. Timing at 10¹¹ (measured; s24-30, same node, back to back, defaults of int15 + the switch)

Runs alternated off / on / off / on. The command was `ECALC_VERBOSE=1 RNS_VERBOSE=1 ./ecalc 100000000000 [/tmp/…/e11.txt]` under
`srun`. "Wall" is the time around `srun` on the login node: init, the run, the file and the srun launch. Every run made the
defaults' **2 up-corrections** (the division's window + corrections: 1.22–1.26 s).

With the digit file (job 21489, 02:03–02:28 EDT; sha1 of each file by `dd iflag=direct | sha1sum` against `~/V214/e_1e11.sha1`):

| run | T1 s | total s | dc s | writer thread's write s | **wall, with the write** s | digits |
|---|---|---|---|---|---|---|
| off 1 | 23.72 | 241.78 | 60.34 | 60.23 | 304.4 | identical to e_1e11.out |
| on 1 | **0.00** | **216.68** | **22.51** | 55.56 | **241.3** | identical |
| off 2 | 22.84 | 243.53 | 60.67 | 60.56 | 306.3 | identical |
| on 2 | **0.00** | **217.59** | **23.12** | 55.83 | **242.9** | identical |
| **mean off → on** | 23.3 → 0.0 | **242.7 → 217.1 (−25.5, −10.5 %)** | 60.5 → 22.8 | 60.4 → 55.7 | **305.4 → 242.1 (−63.3 s, −20.7 %)** | |

Without a digit file (job 21495, 02:28–02:43 EDT; the digits still formatted, T2-windowed and residue-checked). This is the
"without the disk write" wall of D3:

| run | T1 s | total s | dc s | **wall, without the write** s |
|---|---|---|---|---|
| off 1 / 2 | 0.67 / 0.67 | 216.79 / 214.99 | 6.16 / 5.44 | 225.2 / 222.5 |
| on 1 / 2 | 0.00 / 0.00 | 215.56 / 216.67 | 0.01 / 0.01 | 217.7 / 218.7 |
| **mean off → on** | 0.67 → 0 | 215.9 → 216.1 (equal within noise) | 5.8 → 0.0 | **223.9 → 218.2 (−5.7 s)** |

Reading (measured): with the file, the switch removes the whole cost V2 found. T1 no longer waits for the writer (−23.3 s of
`total`), and the file is not written twice (dc 60.5 → 22.8 s). The patch itself takes 0.002 s. The end-to-end gain is 63 s:
the "with the write" wall is now 25 s above `total` instead of 63 s. Without the file, the off path loses only the redone
formatting (≈ 6 s), and the switch removes it.

#### 6. Verdict and the target

- **Recommend adopting `ECALC_CORR_PATCH=1` as the default** (the user's decision). At 10¹¹ with the digit file it saves
  **−25.5 s of `total` and −63 s end to end** (measured, 2 + 2 runs). Every output was byte-identical: 4 × 10¹¹ files against
  e_1e11.out's sha1, and all e9, e8 and chain runs. The gates pass with the switch off, and the forced-correction runs pass with it
  on. It also removes an in-place race on the off path: `db_add_small` on the device X while the writer thread reads it (harmless
  today only because the file is then rewritten).
- **Size > 1 / the 576-node target: no gain on the current code.** The distributed division corrects before the part files are
  written (§1), so there is no rewrite to avoid. `=2` is neutral: the patch at sizes 2 and 4 took 0.008–0.023 s. It is the piece
  IO's streamed multi-node writer (W5) needs (§2.1). Once that writer starts before the corrections, `=2` avoids a 74 GB rewrite
  per affected node: 90–120 s at the Lustre rates (modelled, PLAN §36). Without `=2`, the writer must join and rewrite.
- 576-node estimate at this writing (unchanged by this work): 4.25 × 10¹³ digits in ≈ 4.1 min at an assumed 2 GB/s per node, and
  5.3–5.8 min at the measured single-stream Lustre rate (modelled, PLAN §36.10).

#### 7. Open issues

- Not run: the protocol's 4 × 10¹⁰ gate (`e_4e10.out`). Four runs at 10¹¹ were compared instead, all identical: two off and two
  on, the on runs through the patch path.
- `t_mn_grid` and the `full` / `stress` steps of mnaccept were not rerun with the switch on. The switch does not touch them.
- The chains in the end-to-end tests are ≤ 11 digits (e's digits allow no longer at these sizes). Multi-limb and multi-part chains
  are covered by `t_patch` alone.
- Coordination with IO: a packed output format (W2) changes the byte image the patch writes (`mn_out_tail_core`, one place, §3).
  A streamed multi-node writer (W5) must set `o->t2_defer` and run with `newton_x_defer` (§2.1).
- The 10¹¹ runs were on one node (s24-30) only. The wall includes srun's launch.

#### RESUME

- 2026-09-26 22:50: code committed (e19a7b7, bd39e08); built on aac6 `~/ntt-K15`; `tests/t_patch` passed on the login node
  (5165 checks, 0 failures, both with ECALC_ODIRECT=0 and =1 — the O_DIRECT patch path exercised).
- 22:46: `~/k15tmp/chain.sh` launched on aac6 (setsid): `k15_batch.sh A B C D a08c271` in turn, each waiting for Stage 0,
  one 45-min `-J K` job each (the job id in `~/k15tmp/<stage>.job`), logs in `~/k15tmp/<stage>/` (`batch.log` = the summary).
  To resume: read the batch.logs; a stage not done: `cd ~/ntt-K15/ecalc && ./k15_batch.sh <stage> a08c271` (setsid nohup).
- 2026-09-27: all four batches completed overnight: A 23:38–00:06 (job 21468), B 01:36–01:42 (21477), C 02:03–02:28 (21489),
  D 02:28–02:43 EDT (21495; "cancelled" = the batch's own scancel at its end). Results in §4–§6. No job of K is left.
  **DONE.** Remaining only as in §7 (the 4e10 gate, if the integrator wants it: `./k15_batch.sh` has no stage for it;
  `R "./ecalc 40000000000 /tmp/x"` with `ECALC_CORR_PATCH=1` and a cmp against `results/e_4e10.out`).

## M615 — Phase 15 Batch 2, agent M6: item 6 (E11, band-sized products and the chunked division window) with the Q/S margin trim

(former file: M615.md)

Branch `p15-M6` from `main` f184d51 (B1). Phase 1 only: the feasibility note (PLAN §36.5 row 2c, §37 row 4). No code change, no node job.
Files: this report, `tests/m6_terms.py` and `tests/m6_e11.py` (throwaway models on `mem_model.py`), `results/M615/` (the outputs; the two
`BS_LAYOUT_ONLY` logs from the aac6 login node, clone `~/ntt-M615` at f184d51). Every number is labelled **measured**, **modelled** or
**assumed**. Written 2026-09-27, 18:20 EDT.

#### 1. The feasibility note

##### 1.1 What reserves the memory today (B1)

The arena is mapped at init as max(the bs regions, the dm need, the tree need) per APU (`binsplit.c layout_arena`). The dm need is
`dm_layout`'s max(v2, v3) (+ the top product's scratch at g > 1). The C layout, `BS_LAYOUT_ONLY` on the login node (**measured** output
of the formulas, `results/M615/layout_np4.txt`, ECALC_NP=4; np 3 gives the same arena):

| per APU unless named | 10¹¹, one node | 1.4 × 10¹¹, one node | the target share (4.25 × 10¹³ / 576) |
|---|---|---|---|
| n = one n_Q share's quarter | 11.11 GB | 15.56 GB | 8.20 GB (u) |
| Q, S reserved (qp = n + 10 %) | 12.22 | 17.11 | 9.02 |
| the hole = max(t = A_h μ at 2k + 8, t1 + r) | 22.57 (both 2.0 n) | 31.60 | 16.65 |
| v2 (reciprocal / division set) / v3 (top bs level + hole) | 63.49 / **63.82** | 86.74 / **89.35** | **48.26** (+ 12.93 scratch) / 47.09 |
| node: bs regions / dm need / tree need / **arena** | 261.2 / 255.3 / — / **273.6** | 319.8 / 357.4 / — / **357.4** | 185.2 / **244.7** / 224.8 / **244.7** |

So at 10¹¹ the arena is almost the bs regions' (dm adds 12.4 GB); at 1.4 × 10¹¹ and at the target the dm need sets it. The target's floor
is the tree need (224.8 GB): **at most 19.9 GB per node can be saved there**, whatever is done to dm.

**The 10 % margin is on the formula, not on the blocks**: the top-level P, Q (one node) are `rns_mul_dist_db` results reserved at na + nb + 8
(`mul_grid`), S = P + Q is formed in P's block, the mn P, Q are mdbs of basis ≈ n_Q. The one real use of a margin is v3's local top level
at g > 1: the last node's local Q is **1.036 ×** the average share at the target (modelled, lgamma of its term range); at one node the
top level's inputs sum exactly to the output (the 0.516 / 0.484 split cancels in P1 + P2 + Q1 + Q2).

**Measured check of the one-node accounting** (V3's run `results/V314/e11_def.log`, 10¹¹): the pool holds 55.56 GB per APU live through the
reciprocal = P + Q + r (jl + 4) + r2 (2 jl + 4) + t1 (take + jl + 10) = 5.0 n; window peak **59.72** (+ a 2³¹ piece, 4.2 GB); division
window peak **58.73** (Q, S, μ, t = 2.0 n, or Q, X, Aw, xq = 2.0 n, + a piece). My count gives 59.7 for both (modelled).

##### 1.2 Which buffers shrink, and by how much

**One node** (`newton_db.c recip_db2`, `newton_db_divmod_shifted`; `rns_dist.c mul_grid`):

| buffer | today | after | exact? |
|---|---|---|---|
| t1 = Q_t r (the middle product under `NEWTON_RECIP_MID`) | reserved take + jl + 10 = **1.5 n**; `mul_grid` reserves the whole na + nb + 8 even for `rns_mul_band_db` and truncates after | stored mod B^(take+4) as it is accumulated: max(take + 4, 2 jl + 3) + 8 = **1.0 n** (also holds r·\|d\| and a whole round's u); 0.5 n if only the read band [take − j − G, take + 4) is kept | mod B^w: bit-identical; the lower band: a unit in u, as `NEWTON_RECIP_CUT` |
| t = A_h μ (the division's high product) | 2k + 8 = **2.0 n**; only t >> (k + 1) is read | stored from limb k + 1 − G: **1.0 n** | X low by ≤ 1 with probability ~ pieces / B^G; the correction loop restores it (as the low cut B3 does today) |
| the hole = max(t, t1 + r) | 2.0 n | **1.5 n** (1.0 n with the band-only t1) | — |
| xq = low_w(X Q) | nq + k + 8 = **2.0 n** | chunked: R = Aw − Σ pieces mod B^w in place, **0** (the piece is the chunk); or stored at w + 8: 1.0 n | bit-identical (R mod B^w; the sign from the top limb, as the host path) |
| Q, S, v3's top-level terms (the trim) | 1.1 n each; v3 4·in + out = 3.3 n | 1.0 n + 10 limbs; 3.0 n | sizing only |

Live peaks per APU at 10¹¹ (modelled): reciprocal 59.7 → **54.2** GB (4.5 n + piece), division 59.7 → **48.6** (hi 4.0 n + piece; lo 3.0 n + piece).
**All three parts are needed**: without t stored high-only the hole stays 2.0 n and v2, v3 do not move — the value-identical subset (t1 mod
B^w, xq at w, the trim) gives exactly the trim's numbers (`tests/m6_e11.py`, row T+Bid+Ww).

**The target (576)**: the mn path already has what E11 adds on one node — `mn_grid` delivers the middle product in basis take + 4
(R4) and the low product in basis w — so band-sized t1 and the chunked window save **nothing real** there. What the target reserves is
`dm_layout`'s one-node formula scaled by 1/g. Counted from `recip_mn` / `newton_mn_divmod` (modelled; `tests/m6_e11.py --mn`; u = 8.20 GB,
the product scratch 12.93 GB per APU):

| moment of the mn path | now | after | change |
|---|---|---|---|
| recip, r·\|d\| product: P Q r t_old d + C | 5.0 u + scratch = 53.9 | 4.0 u + scratch = 45.7 | free t once d is formed (t is dead: u, d are copies) |
| recip, r′ formed: P Q r t corr rs | 5.5 u = 45.1 | 4.5 u = 36.9 | free t once corr is formed |
| **division, A_h μ: S Q μ A_h + t (basis 2k)** | **6.0 u + scratch = 62.1** | 5.0 u + scratch = 53.9 (4.0 u = 45.7 with t from k + 1 − G) | A_h as a view of S (`mdb_view` at the offset) instead of an `mdb_shift` copy |
| division, low product / window | 4.0 u + scratch = 45.7 / 5.0 u = 41.0 | unchanged | — |

Against the reservation of **61.18 GB per APU** the division's A_h μ moment already sits **~0.9 GB per APU above** it today (modelled, within
the model's slop; the VMM pool or hipMalloc would take it). With the two value-identical edits the mn peak is 53.9 < the tree floor 56.2,
so a g > 1 branch of `dm_layout` that describes the mn path lets the arena fall to the tree need.

**Totals** (`tests/m6_e11.py`, B1 defaults; modelled):

| variant | 10¹¹ node | 1.4 × 10¹¹ node | one-node ceiling 480 / 502 GB | target node | 576 ceiling (480 GB) |
|---|---|---|---|---|---|
| as built (B1) | 410.4 | 495.0 (does not fit) | 1.34 / 1.43 × 10¹¹ | 416.0 | 9.67 × 10¹⁰ per node = 5.57 × 10¹³ |
| T, the trim alone | 403.4 | 474.0 | 1.42 / 1.48 | 409.5 | 5.75 × 10¹³ |
| B (t1 1.0 n + t high-only) | 405.0 | 476.4 | 1.41 / 1.48 | 411.7 | 5.69 × 10¹³ |
| W alone | 410.4 | 495.0 | 1.34 / 1.43 | 416.0 | 5.57 × 10¹³ |
| **T + B + W** | **398.0** (arena = bs regions) | **457.4** (arena = bs regions) | **1.48 / 1.57** | **396.1** (arena = tree) | **6.0 × 10¹³** |
| T + B0 + W (t1 band-only) | 398.0 | 457.4 | 1.48 / 1.57 | 396.1 | 6.0 × 10¹³ |

With T + B + W the one-node arena reaches the bs regions at every size up to the ceiling, so the band-only t1 (B0) buys nothing more; the
next binding term is the bs regions (grown by `BS_SEED_FILL=128`), then the planes. V3's 1.41 → 1.52 was on B0's layout; on B1 it is
1.34 → 1.48 × 10¹¹ (480 GB). Removing the division's measured VMM growth (16.3 GB at 10¹¹ with the fill; agent AS's item) does not move
either ceiling (modelled: the arena at init binds).

##### 1.3 Code changes (phase 2, behind a switch, off by default)

Switch `DM_BAND=1` (the one-node t1 / t / window stores; the mn edits; the layout terms) and the trim under the same switch (no separate
switch is needed: it only sizes).
- `newton_db.c` (mine): `recip_db2` reserves t1 at max(take + 4, 2 jl + 3) + 8 and asks the band store; `newton_db_divmod_shifted` asks the
  high-only store for t and the subtracting store for the window, the sign and the corrections as the host path; `recip_mn` frees t twice
  earlier; `newton_mn_divmod` passes A_h as a view.
- `rns_dist.c` (NP's and G5's file this batch): `mul_grid` gains an output window [c, w): the reservation min(nc, w) − c + 8, the first piece
  and every add clipped to it (a truncating `db_add_shifted` in `dbig.c`, carries out of the top dropped as `rns_mul_band_db` does today),
  and a subtract mode into an existing number (the chunked window); `rns_mul_dist_mn_cut` with view offsets (a wrapper; `mn_grid` takes
  views already). Optional for the target: `mn_grid` with a lower basis start c (t from k + 1 − G at g > 1).
- `binsplit.c dm_layout` (AS's): the terms of §1.2 under the switch, and a g > 1 branch of the mn moments; `mem_model.py` (AS's) the same,
  `--check-c` exact.

##### 1.4 Exactness

- t1 stored mod B^(take+4): the same value `rns_mul_band_db` returns today (it truncates after; a skipped piece at oa + ob ≥ w is a multiple
  of B^w, a formed piece's limbs at or above w vanish mod B^w): **bit-identical** mu. The optional lower band (0.5 n) would add the low cut's
  kind of error (≤ pieces units at B^(take − j − G)) — not needed (§1.2: B0 buys nothing on B1).
- t stored from k + 1 − G: each formed piece drops < B^(k+1−G); the sum drops < pieces · B^(k+1−G), so floor(t / B^(k+1)) is lower by at most
  one, and only when t's fractional part lies within pieces / B^G of an integer (G = 2: ~10⁻³⁵ per run). X is then low by one; the division's
  loop (R = A − X Q computed exactly over w limbs, then ±Q until 0 ≤ R < Q) restores it — the invariant that already covers the low cut
  (NEWTON_HIGHPROD) and ECALC_TEST_CORR. **Digits byte-identical by construction; a correction costs a tail patch (ECALC_CORR_PATCH=2)**.
- The chunked window: R mod B^w is the same integer as Aw − xq mod B^w; |Aw − xq| < 65 Q < B^(w−1), so the top limb gives the sign:
  bit-identical R and X.
- The mn edits (free t earlier, A_h as a view) and the trim: no value changes.

##### 1.5 Risks

1. **Placement** (medium): DM_TIGHT's hole relies on where the VMM pool puts t1, r, t (jobs 21131–21154 fell back to hipMalloc each time a
   size changed). AS is reworking the division's two large blocks now; this must follow AS and reuse its placement.
2. **File overlap**: `rns_dist.c` (NP, G5), `binsplit.c dm_layout` and `mem_model.py` (AS). Phase 2 should start after NP and AS merge.
3. **The target's mn peak is not measured** (no size > 1 `ECALC_LIVE` log exists; searched the repo and aac6). The division's A_h μ moment is
   counted at 62.1 GB per APU against 61.18 reserved: a 2-node run at 10¹⁰–4 × 10¹⁰ per node with `ECALC_LIVE=1` (≈ 1 node-hour) should
   measure the u-count before any g > 1 layout is shrunk — worth doing even if nothing else here is built.
4. A t high-only X that is one low costs a correction (seconds, the patch path); the probability is negligible with G = 2.
5. Time: none expected (the same pieces are formed; the truncated adds move fewer limbs; one fewer copy of A_h) — **assumed**, to be
   measured at 10¹¹ (a paired series) if built.

##### 1.6 Does memory matter at the target? What the saving buys

At the target the node is **416 GB of 480** (modelled, B1 launch line) and the digits are limited by the **grid step at 4.29 × 10¹³**, not
by memory. The saving (−19.9 GB per node, 416 → 396, modelled; bounded by the tree need) buys **margin** (64 → 84 GB) and a memory
ceiling of 6.0 × 10¹³ instead of 5.57 × 10¹³ that is usable only past the grid step, i.e. at the step's time cost. It buys no digits and no
time at the planned target. On one node it buys **1.34 → 1.48 × 10¹¹ at 480 GB** (1.43 → 1.57 at 502; +10 %), and makes 1.4 × 10¹¹ fit
(495 → 457 GB).

##### 1.7 Conclusion (for the integrator and the user)

- **The one-node E11 items (band t1, t high-only, chunked window, the trim)**: worth building **only if one-node runs above 1.34 × 10¹¹
  are wanted** — they give +10 % one-node ceiling, nothing at the target (the mn path already has the band and the window). 3–4 days,
  medium risk, after AS and NP merge. **For the target: not worth it.**
- **The target part** is small and different from E11: two value-identical edits in `newton_db.c` (free t earlier in `recip_mn`, A_h as a
  view in `newton_mn_divmod`) plus a g > 1 branch of `dm_layout` = −19.9 GB per node of margin. Recommended first step, cheap: measure the
  mn dm peak on 2 nodes with `ECALC_LIVE=1`, because the count says the division's A_h μ moment is already at the reservation.
- My recommendation: **stop E11 as specified**; offer the user (a) the 2-node live measurement (≈ 1 node-hour) and (b) the two mn edits +
  the g > 1 layout branch (≈ 1 day, after AS), and keep the one-node items on the shelf unless > 1.34 × 10¹¹ on one node is wanted.

#### 2. Tests and commands (all on the login nodes, no GPU)

| what | command | result |
|---|---|---|
| the C layout, ECALC_NP=4 | aac6 `~/ntt-M615/ecalc` (f184d51, built with `make -s -j16`): `ECALC_NP=4 ECALC_VERBOSE=2 BS_LAYOUT_ONLY=1e11:1,1.4e11:1,1.52e11:1,73784722222.2222:576 ./ecalc 42500000000000 /dev/null` | `results/M615/layout_np4.txt`: target dm_need 61 183 493 712 B per APU, tree 56 200 005 237, arena 244.74 GB, node 387.21 GB at 2³¹ (C's own node line; the model's 416.0 adds the host and the pool) |
| the same, three primes | `ECALC_NP=3 BS_LAYOUT_ONLY=1e11:1,1.4e11:1,1.52e11:1 ./ecalc 1000000 /dev/null` | `results/M615/layout_np3.txt`: the same arenas |
| the terms | `python3 tests/m6_terms.py` | `results/M615/m6_terms.txt` (the port agrees with the C lines: v2, v3, hole, need at the printed precision, 10¹¹, 1.4 × 10¹¹ and the target) |
| the variants | `python3 tests/m6_e11.py`, `--nogrow`, `--mn` | `results/M615/m6_e11.txt` (§1.2) |

#### 3. Open issues

- The mn dm peak at size > 1 is not measured (§1.5 item 3).
- The model's one-node division growth (VMM_DM_GROW_FILL, 16.3 GB measured) is AS's; whether smaller division blocks also remove it is not
  known (assumed: they help).

#### 4. Outside my files

Nothing edited outside `results/M615*` and `tests/m6_*`. aac6: clone `~/ntt-M615` (built), `~/M615/` (the two layout logs).

#### RESUME

Phase 1 done (this note, committed). Waiting for the integrator / the user. If resumed for phase 2: start from `p15-M6` after AS
(`dm_layout`, `mem_model.py`) and NP (`rns_dist.c`) merge into main; rebase; build in the order of §1.7 — (1) the 2-node `ECALC_LIVE=1`
measurement of the mn dm peak, (2) `recip_mn` / `newton_mn_divmod` edits + the g > 1 `dm_layout` branch under `DM_BAND=1`, (3) only if asked,
the one-node stores in `mul_grid` (§1.3). Gates per the private protocol; `mem_model.py --check-c` exact.

## O2 - the reciprocal-gather stall: why `db_to_bi` of 0.2 MB takes 2.2 s (2026-10-11/12)

(former file: O2.md)

Task O2 (TASKS 35). Branch `o2` off main 0298fe2, not merged. Labels: measured / modelled / assumed / inferred. Times Eastern.

#### Diagnosis (code reading plus the G1 prelude stamps)

- Measured (G1 item 86, aac7 `~/sess1011c/g1/log/p1.log`, `p2.log`; 4 nodes x 6.441e10): in `mdb_to_host_all` the step `alloc_host -> db_to_bi_done`
  takes **2.20-2.26 s on every node** (p1) and 2.19-2.27 s (p2). All the other prelude steps take < 0.3 ms. The same runs show `db_from_bi(upload)` at
  0.000-0.003 s, whereas in S32 (older code paths) the upload took 0.97-1.0 s on every node.
- Cause (from the code, `ecalc/dbig.c`): host<->device copies go through `bounce_init(d)`, which on its **first use per device allocates two 1 GiB
  pinned host buffers** (`hipHostMalloc` of `BOUNCE` = 2^27 limbs each). A `dbig` spreads its limbs over all 4 quarters (devices), so the first
  `db_to_bi` of a process pins **4 x 2 GiB = 8 GiB**. In a multi-node run the reciprocal's gather is the first host copy, so a 27 302-limb
  (0.2 MB) share pays the whole 8 GiB pinning. 8 GiB in 2.2 s = 3.6 GiB/s, the rate of page zeroing plus pinning (modelled, consistent with measured).
  The cost moves to whichever call is first: in S32 the upload came first and paid it (about 1 s), now `db_to_bi` does and the upload is free.
  This is not algorithmic (no quadratic conversion, no per-limb host work): the copy itself is 0.2 MB.
- The late node (inferred, to be confirmed by the item below): pinning 8 GiB needs 8 GiB of free pages at once. A node whose free memory must
  first be reclaimed from the page cache (hold start: Cached 84-160 GB, C10/T09) or compacted pins more slowly, so one node enters the allgather
  2.6-3.7 s late (G1) and, in bad cases, 15-19 s (C10, 4 nodes) or +77..+151 s (10 nodes, same stage). This matches the hold-start and episode
  pattern of C10 (a per-node memory state, not the fabric) and the 20-25 s of host aac7-n01 in the same call (S32). `DBIG_BOUNCE_TS=1`
  prints each pinned allocation's time per device, which tests this directly.

#### Change (behind a switch, off by default)

- `DBIG_SMALL_BOUNCE=1` (`dbig.c`): a quarter of at most 2^20 limbs (8 MiB) in `db_to_bi` / `db_from_bi` goes through one 8 MiB pinned buffer per
  device (allocated on first use, same non-blocking stream, same synchronisation order), so the gather pins 4 x 8 MiB instead of 4 x 2 GiB. Same
  bytes copied, so digits are identical by construction; the 1 GiB buffers are still allocated on the first large copy (none in the multi-node
  path before the output stage, inferred). A side effect: 8 GiB less pinned host memory per process while no large copy happens (inferred).
- `DBIG_BOUNCE_TS=1`: print only, one `bounce_init dev d: ... <s> s` line per first-use pinned allocation.
- `tests/t_bounce` (`make tests/t_bounce`, not in TESTS): the cold first `db_to_bi` / `db_from_bi` and the steady ones of a small number, per arm.
- `tools/o2_stats.py`: per-run chain interval, per-node `db_to_bi`, the late PE, pin time; paired B - A with 95 % CI and p; `--stop` rule.
- README: rows for both switches; `switches.tsv` / `switches_gen.h` regenerated (`switch_registry.py --check`: 0 problems).
- Built on the aac7 login node (`make -B SHMEM_CRAY=1`, rc 0, `-DCOMM_SHMEM` present; `tests/t_bounce` built). No local hipcc.

#### Tests armed (fire-and-forget; nothing measured yet)

| where | what | marker | expected |
|---|---|---|---|
| aac7 queue `~/aac7q/queue/70_o2_bounce.sh` (4 nodes, EST 60 min, APPROVED) | gate.sh --build --nodes 2 off and --env DBIG_SMALL_BOUNCE=1; 1 node t_dbig on + t_bounce off/on x3; 4-node AB/BA pairs (A off, B on) with `COMM_HS_TS=1 NEWTON_MN_RECIP_TS=2 DBIG_BOUNCE_TS=1`, per-node MemFree/Cached; stop: >= 4 pairs and p < 0.05 on the chain, or 6 pairs, or 50 min | `~/o2/O2_DONE` | after items 50 and 60 (about 03:30-04:30 ET, assumed) |
| aac7 `~/aac7q/queue/90_o2_c45.sh` (installed by item 70 only if both gates PASS, t_dbig OK, every run VERIFY OK and chain B - A < -1 s) | C4 `COMM_SHMEM_RPIPE` and C5 `MN_REDIST_FUSE` re-time with `DBIG_SMALL_BOUNCE=1` in every arm, rounds A/C4/C5 rotating, cap 3 rounds / 38 min | `~/o2/c45/C45_DONE` | after item 70 |
| aac6 driver pid 2949459 (`~/o2/o2_aac6.sh`, starts 03:31 ET on aac6-spx-b, -t 40 min) | gate.sh --build --nodes 1 --line none off and on; t_dbig on; t_bounce 27 302 limbs x5 per arm, 109 206 limbs x2 per arm | `~/o2/O2_AAC6_DONE` | about 04:00 ET |

Copies of the item/driver scripts: `tools/o2_items/`.

#### Collect

- aac7: `ssh aac7 'cat ~/o2/summary.txt; cat ~/o2/c45/summary.txt 2>/dev/null; grep -h bounce_init ~/o2/log/o2_p1_A.log | head -8'`
  (detail: `python3 ~/ntt-wt/o2/tools/o2_stats.py ~/o2/log`; per-node memory `~/o2/mem.tsv`; item status `~/aac7q/queue/70_o2_bounce.rc`).
- aac6: `ssh aac6 'cat ~/o2/summary.txt; cat ~/o2/unit.txt'`.
- Reading: A-arm `bounce_init ... 2 x pinned` lines give the pin time per device and node; the late PE in A runs should be the node with the largest
  pin time and the lowest MemFree in `mem.tsv`. B arm: chain interval should drop by about 2.2 s (assumed from the stamps) and the late-PE spread
  should collapse; total 4-node time is too noisy (bimodal, T05) to resolve 2 s in 6 pairs, so the chain interval is the primary metric.

#### Open issues

- The late-node mechanism is inferred, not yet measured; the item's pin-time lines and memory snapshots decide it.
- The big buffers are still allocated lazily on the first large copy (single-node paths, output); if that ever lands in a timed multi-node
  stage it would show there. A later option: size `BOUNCE` by the copy or pre-pin at init, off the critical path.

#### Collected results (aac7 queue item 70, 4-node hold, 2026-10-11 23:17-23:55 ET; all numbers measured)

| test | result |
|---|---|
| gate, 2 nodes, 1e9, switch off | PASS: VERIFY OK, digits identical, segv 0, ecalc total 20.50 s |
| gate, 2 nodes, 1e9, `DBIG_SMALL_BOUNCE=1` | PASS: VERIFY OK, digits identical, segv 0, ecalc total 21.83 s |
| `t_dbig` with `DBIG_SMALL_BOUNCE=1` | VERIFY OK (555 checks) |
| BOUNCE unit (27302 limbs), switch off, 3 runs | `first_to_bi` 9.95 / 8.15 / 7.96 s; steady `to_bi` 0.0009 / 0.0005 / 0.0004 s |
| BOUNCE unit, switch on, 3 runs | `first_to_bi` 0.65 / 0.35 / 0.39 s; steady `to_bi` 0.00006 s; ok 11/11 each |

4-node paired AB/BA chain (A = default, B = `DBIG_SMALL_BOUNCE=1`), 4 pairs, stop rule met after pair 4, every run VERIFY OK:

| pair | A total (s) | B total (s) | A chain (s) | B chain (s) | A `db_to_bi` [min,max] (s) | B `db_to_bi` [min,max] (s) |
|---|---|---|---|---|---|---|
| 1 | 248.49 | 216.51 | 10.48 | 0.07 | [2.118, 10.431] | [0.014, 0.018] |
| 2 | 237.34 | 214.95 | 12.47 | 0.06 | [2.240, 12.413] | [0.014, 0.016] |
| 3 | 241.55 | 206.81 | 9.14 | 0.06 | [2.215, 9.081] | [0.014, 0.016] |
| 4 | 229.58 | 198.36 | 12.09 | 0.06 | [2.240, 12.033] | [0.015, 0.016] |

- Paired B - A, **chain: -10.98 s, 95 % CI [-13.43, -8.54], p 0.0007**; **total: -30.08 s, 95 % CI [-38.59, -21.58], p 0.0015** (n = 4, measured).
  The late rank in A is a different node each pair (nodes 0, 1, 3, 3; +6.9 to +10.2 s); in B it is at most +0.014 s. Sum of `pin` time over all ranks: A 18.6-25.7 s, B 0.06 s.
- Total gain (30.1 s) is larger than the chain gain (11.0 s): the remainder is not explained by the chain alone (the `dm` phase is 87-95 s in B vs 109-115 s in A; the `bs` phase 74-83 s vs 82-84 s); likely
  the same pinning cost elsewhere in the run, not yet attributed (assumed). Run order within the item makes pair-to-pair drift visible: totals fall from pair 1 to 4 in both arms.
- Per-node memory snapshots (`MemFree` / `Cached` per node, taken per run, `~/o2/mem.tsv` on aac7): A runs 385.5-440.1 GB free, B runs 413.1-440.1 GB free; no node in either arm shows a memory shortage. The snapshots differ mostly by pair index (the page cache shrinks with time), not by arm, so they do not show a memory effect of the switch.
- Item 90 (`90_o2_c45.sh`, the C4/C5 re-time: B = `COMM_SHMEM_RPIPE=1` and B = `MN_REDIST_FUSE=1`) **was installed** at the end of item 70 and is running; its results are not included here (summary `~/o2/c45/summary.txt`, marker `~/o2/c45/C45_DONE`).
- Verdict: the switch removes the stall (pinned pages 4 x 2 GiB at the first `db_to_bi`): chain -11 s and total -30 s on 4 nodes, digits identical on the gates. Adoption is the user's decision; the 2-node gate and unit tests pass.

#### Collected aac6 part (2026-10-12; measured)

Re-armed driver `O2_AAC6_DONE` (non-empty marker file written at 05:42 ET; the earlier 03:00 hold had been cancelled after 10 min pending, see the git log). 1 node, one hold started 05:37 ET (PPAC SPX), line=none, no multi-node.

| test | result |
|---|---|
| gate 1 node 1e9, switch off | PASS: VERIFY OK, digits identical, segv 0, ecalc total 12.57 s |
| gate 1 node 1e9, `DBIG_SMALL_BOUNCE=1` | PASS: VERIFY OK, digits identical, segv 0, ecalc total 11.99 s |
| `t_dbig` with the switch on | VERIFY OK (555 checks) |
| `t_bounce` 27 302 limbs, off, 5 runs | `first_to_bi` 1.278-1.448 s (mean 1.366); steady 0.14-1.7 ms; pinned 2 x 1 GiB per device 0.25-0.38 s |
| `t_bounce` 27 302 limbs, on, 5 runs | `first_to_bi` 0.155-0.180 s (mean 0.162); steady 0.056 ms; pinned 8 MiB per device 0.02-0.03 s |
| `t_bounce` 109 206 limbs, off / on, 2 runs each | `first_to_bi` 1.281, 1.324 s (off) vs 0.157, 0.160 s (on) |

- All `t_bounce` runs ok 11/11, VERIFY OK. The first `to_bi` falls by 1.2 s (8.4 x for 27 302 limbs) on aac6; aac7 shows the same pinning at a larger cost (unit: 7.96-9.95 s off vs 0.35-0.65 s on, item 70). No aac6 multi-node run exists (no Cray SHMEM).

#### Collected aac7 6-node paired run (queue item `o2b`, 2026-10-12 01:02-01:38 Pacific, 6 nodes; all numbers measured)

Independent repeat of the 4-node item 70 on a different node set (6 nodes, 8 runs, 4 AB/BA pairs, stop rule met after pair 4, every run VERIFY OK; A = default, B = `DBIG_SMALL_BOUNCE=1`, binary of rev 2f8de94c):

| pair | A total (s) | B total (s) | A chain (s) | B chain (s) | A `db_to_bi` [min,max] (s) | B `db_to_bi` [min,max] (s) | late rank in A (+s) |
|---|---|---|---|---|---|---|---|
| 1 | 268.09 | 247.37 | 9.09 | 0.06 | [2.152, 9.005] | [0.014, 0.016] | node 1, +6.86 |
| 2 | 258.83 | 247.51 | 9.64 | 0.06 | [2.166, 9.593] | [0.014, 0.017] | node 5, +7.42 |
| 3 | 248.84 | 254.78 | 2.28 | 0.06 | [2.154, 2.230] | [0.014, 0.015] | node 1, +0.08 |
| 4 | 248.30 | 256.04 | 4.13 | 0.08 | [2.135, 4.080] | [0.014, 0.040] | node 1, +1.94 |

- Paired B - A, **chain: -6.22 s, 95 % CI [-12.02, -0.42], p 0.042** (n = 4); **total: -4.59 s, 95 % CI [-26.49, +17.31], p 0.55** (not resolved).
- The effect on the chain is the same mechanism as at 4 nodes: `db_to_bi` is 2.1-9.6 s with the switch off (the first call pins 4 x 2 GiB per node; the slow rank is a different node each pair) and 0.014-0.040 s with it on; sum of pin time over all ranks A 13.0-33.9 s, B 0.09-0.11 s. In pairs 3 and 4 the A stall is small (2.3 and 4.1 s) and the totals are 5.9 s / 7.7 s slower in B, so the total difference is dominated by run-to-run variation (totals 247-268 s; the NIC launch group differed between runs, fgfc 2.00-2.40 e9 counts, no pattern with the arm).
- Compare with the 4-node item 70: chain -10.98 s (CI [-13.43, -8.54]) and total -30.08 s (CI [-38.59, -21.58]). At 6 nodes the chain gain is smaller (-6.2 s) and the total gain is not detectable. The total gain at 4 nodes was larger than the chain gain and not attributed there; the 6-node data do not repeat it, so it should not be quoted as a general result.
- Memory: per-node MemFree 354.6-505.5 GB over all 48 snapshots (6 nodes x 8 runs); no shortage in either arm (the spread follows node and page cache, not the switch).
- Verdict: with the switch on, the pinning stall is removed in every run on both clusters and digits are identical on the gates; the benefit is 6 to 11 s on the reciprocal chain at 4-6 nodes (with a node-dependent size, since the late rank varies from 0 to +10 s), and a total saving is shown at 4 nodes only. Adoption is the user's decision.

#### Collected C4 / C5 re-time with the stall fix on (queue item 90, 4-node hold, 2026-10-12 03:29-04:02 ET; all numbers measured)

- Setup: 4 nodes x 6.441e10 digits, aac7 line + `COMM_XSTATS=1 MN_WAIT_STATS=1 DBIG_SMALL_BOUNCE=1` in every arm, no digit writes; 3 rounds of three runs
  (A = fix only, C4 = + `COMM_SHMEM_RPIPE=1`, C5 = + `MN_REDIST_FUSE=1`; rotating order A C4 C5 / C5 A C4 / C4 C5 A). All 9 runs rc 0, `all 4 nodes: VERIFY OK`.
- Totals (s): round 1 A 231.47, C4 226.37, C5 213.15; round 2 A 203.46, C4 211.28, C5 216.93; round 3 A 195.83, C4 198.58, C5 198.35.
- Reciprocal chain interval is 0.06-0.09 s in every run (it was about 11 s with the stall), so the fix holds with both switches on.
- Paired B - A (n = 3 rounds), C4 `COMM_SHMEM_RPIPE=1`: chain -0.010 s (95 % CI [-0.035, +0.015], p 0.23); **total +1.82 s, 95 % CI [-14.35, +17.99], p 0.68**.
- Paired B - A (n = 3 rounds), C5 `MN_REDIST_FUSE=1`: chain -0.010 s (95 % CI [-0.053, +0.033], p 0.42); **total -0.78 s, 95 % CI [-40.89, +39.34], p 0.94**.
- Verdict: no measurable effect of either switch (both CIs include 0 and are wide, +/-15 s and +/-40 s). The totals fall by 35 s from round 1 to round 3 in
  every arm (231 -> 196 s for A), a slow time-of-run drift that dominates the pairs; three rounds cannot resolve a gain of a few seconds. Neither switch is
  supported for adoption on this data; the earlier C03 result (no effect) stands. A larger sample would need the drift controlled (same hold, more rounds).

#### RESUME

- Branch o2 (pushed): dbig.c switch + print, tests/t_bounce.c, Makefile rule, README rows, registry, tools/o2_stats.py, tools/o2_items/, this file.
- aac7: worktree `~/ntt-wt/o2` (built), outputs `~/o2/`; item 70 queued; item 90 staged at `~/o2/90_o2_c45.sh`. aac6: clone `~/ntt-o2` is created by the
  driver at 03:31 ET; remove it and `~/o2` after collection. After collection: fill the results here, then the coordinator decides adoption.
- 2026-10-12: item 90 (C4/C5) collected above. Queue items `tools/o2_items/7c_o2c_two.sh` (DBIG_SMALL_BOUNCE off/on at 2 nodes, output `~/o2c/`, marker
  `~/o2c/DONE`, stats `~/o2c/stats.txt`) is armed; collect with `ssh aac7 'cat ~/o2c/summary.txt'` and add a "2-node" section here.

## O5: K7-a GPU-3 rebalance -- the B5 form (B4 with APU 3 given a share of the inverse)

(former file: O5.md)

Branch o5 (from main 0298fe2). TASKS.md items 39/37; design basis archive:results/C07.md section 3. Switch `RNS_STRATEGY_FORM=B5`
(also `RNS_STRATEGY=B5` for the forced form), **off by default** (auto's form stays B). Labels: **m** measured, **mod** modelled,
**as** assumed. Times Eastern.

#### 1. Design (first implementation, 2026-10-11 night)

B4 today (C07, m on CPX): APUs 0-2 transform 2.5 n points (X whole, Y's lower residue, the whole inverse), APU 3 1.5 n (the
three primes' upper residues, forward only), plus a separate `k_b4_pw` pass with a peer read; B4 was +2.8 % slower than B.

B5 keeps B4's planes, split (`k_bsplit`) and forward; the inverse of each third's 2^logk block (m points, h = m/2, hq = h/2)
runs as independent pieces, which the bit-reversed layout allows: the lower half [0, h) and the two quarters of the upper
half [h, h + hq), [h + hq, m) are complete length-h / length-hq inverse DITs; only the last two DIT stages mix them.

| step | APU p < 3 (prime p) | APU 3 |
|---|---|---|
| forward (as B4) | X (n) + Y lower residue (n/2) = 1.5 n | 3 upper residues (3 x n/2) = 1.5 n |
| inverse I1 | `ntt_inv_pw` lower half against Y's lower spectrum (local, n/2); `ntt_inv_pw` quarter B against APU 3's spectrum (peer read, n/4) | for each prime k: `ntt_inv_pw` quarter A of its own plane against X_k's spectrum on APU k (peer read), 3 x n/4 |
| inverse I2 | `k_b5_merge`: the two last DIT stages (quarter A read from APU 3, n/4 points), the scale 2^-1 2^-1 and, for 3 2^k, the radix-3 inverse stage, in one in-place pass | -- |
| CRT (as B4) | stripes over the four APUs | stripes |

Transform points per APU: 2.25 n / 2.25 n / 2.25 n / 2.25 n (B4: 2.5 / 2.5 / 2.5 / 1.5; B: 3 / 3 / 3 / 0). The pointwise
products are fused into the inverses' b1 pass (`PW_FUSE`), so the peer reads ride on the fused pass (B4's extra `k_b4_pw`
pass over n points is gone); one more pass (`k_b5_merge`, n points read + written) replaces B4's `k_r3_inv` pass at 3 2^k and
is new at 2^k. Peer-read volume per APU: p < 3: n/4 (Y's upper spectrum, quarter B) + n/4 (quarter A inverted) = n/2 (B4:
n/2); APU 3: 3 n/4 (B4: 0). Barriers: one more than B4 (after I1). Memory: identical to B4 (`b_planes` shared), so `auto`
fits B5 exactly where it fits B4.

Scaling: the library's inverse scales by the length^-1 of what it inverts (h^-1, hq^-1); the merge applies 2^-1 after each
of its two stages, so z = m^-1 (sum), then 3^-1 at 3 2^k (as `k_r3_inv`). Every value canonical, so the digits are the exact
residues' CRT: byte-identical to C by construction, and `RNS_STRATEGY_CHECK=1` re-forms every product by C and aborts on a
difference.

Modelled gain (C07 basis, B form 4.4 s of 94 s at 6.441e10 on 1 node): transforms 3 n -> 2.25 n per critical APU and the
`k_b4_pw` pass removed: **-0.6 to -0.8 s per 1-node run (mod)**; at 1e10 (the night's ABBA size) the B-form share is
smaller, so the expected difference is 0.1-0.2 s (mod) against a run-to-run noise of ~0.3 s: the night's ABBA measures the
B-form products' own seconds (`rns_dist: RNS_STRATEGY=... B5 N products X s`) as well as the total.

Files: `ecalc/rns_dist.c` (enum `STRAT_B5`, `k_b5_merge`, the B5 inverse in `b_core`, `BTW_SLOTS` 8 -> 12),
`ecalc/tests/t_strategy.c` (section V compares C, B, B4, B5, auto), `ecalc/README.md` (switch rows), `tools/o5_abba.sh`
(single-node paired ABBA with the stop rule), `tools/o5_aac6.sh` (aac6 driver), `tools/o5_aac7_item.sh` (aac7 queue item 85).

#### 2. Tests (filled in as they complete)

| when | where | what | result |
|---|---|---|---|
| 2026-10-11 23:25 | aac7 login node | `make -B -s -j32 SHMEM_CRAY=1 GMP_HOME=~/gmp ecalc tests/t_strategy` in `~/ntt-wt/o5/ecalc` | rc 0 (m); no GPU run yet |
| 2026-10-11 23:35 | local | `tools/o5_abba.sh` stub dry run (STUB=1): stop rule, pairs.tsv, abba.txt | ok (stub) |
| night | aac6 (driver `~/o5/`, after 03:40) | gate 1e9 B5, t_strategy 28 V, CHECK 1e10 B5, ABBA 1e10 B vs B5 | pending |
| night | aac7 queue item 85 (`~/sess1011c/o5/G/`) | gate 2 nodes + 1 node B5, t_strategy 28 V, CHECK 1e10, ABBA 1e10 | pending |

#### 3. Open issues
- No GPU run before the night's jobs: the first B5 execution is in the drivers (the C07 CPX measurement found B4's math exact,
  B5 reuses its split and forward; the new piece is the quarter inverses and `k_b5_merge`).
- Balance below 2.25 n would need the forward split as well (X's forward is whole on APU p); the CRT and bload are unchanged.
- `k_b5_merge` is a separate pass; fusing it into the last inverse b16 pass (as N3x does for the radix-3 stage) is possible later.

#### 2b. Collected results (aac7 queue item 85, 2 nodes, hold; 2026-10-11 23:55-23:59 ET; measured)

| test | result |
|---|---|
| gate 2 nodes, 1e9, `RNS_STRATEGY_FORM=B5` | PASS: VERIFY OK, digits identical, segv 0, ecalc total 21.18 s |
| gate 1 node, 1e9, B5 | PASS: VERIFY OK, digits identical, segv 0, ecalc total 11.06 s |
| `t_strategy 28 1 V` | rc 0, 1 x VERIFY OK, 0 x FAIL/differs; **B5 products: 8** (nonzero: B5 is used), e.g. logn=28 P=3 LC wall 0.0848 s, LB 0.0649 s |
| CHECK 1e10 B5 (1 node) | rc 0, total 8.90 s, B-form time 0.04 s, VERIFY OK |
| ABBA 1e10, A = B, B = B5, 8 pairs (1 node) | total: mean B5 - B **+0.077 s**, 95 % CI [-0.754, +0.909], p >= 0.05 (t 0.22, sd 0.994); B-form time: mean **+0.027 s**, 95 % CI [+0.024, +0.031], p < 0.05 (sd 0.005) |

- Reading: B5 is correct (every gate and check passes, 8 B5 products in `t_strategy V`) but gives **no wall-clock gain**: total difference +0.08 s with the interval spanning zero; the inverse form itself is 0.027 s *slower* (significant, but 0.03 s of an 8.9 s run). Not adopted on this evidence (the user decides). Single hold, 1 node for the ABBA; the aac6 driver result is separate (not collected here).

#### 2c. aac6 timing: did not run (2026-10-12; measured)

`~/o5/DONE` says "FAILED: hold not running after 25 min": the aac6 hold never started, so the aac6 gate, `t_strategy` and the 1e10 ABBA B vs B5 produced nothing. The only B5 timing is the aac7 gate ABBA (section 2b), which showed no gain. Nothing is pending on aac6 for O5.

#### RESUME
- Branch o5, commits on GitHub; worktree `../ntt-wt-o5`; aac7 worktree `~/ntt-wt/o5` (built on the login node); aac6 clone
  `~/ntt-o5` (bundle from 0298fe2).
- aac6 driver: `~/o5/driver.log`, summary `~/o5/summary.txt`, marker `~/o5/DONE` (ETA about 04:40 Eastern); `~/o5/STOP` stops it.
  Collect: `ssh aac6 'cat ~/o5/summary.txt; tail -3 ~/o5/abba/abba.txt'`; the hold is cancelled by the driver.
- aac7 queue item `~/aac7q/queue/85_o5_gate.sh` (2 nodes, EST 20): `~/aac7q/queue/85_o5_gate.{rc,log}`, outputs
  `~/sess1011c/o5/G/summary.txt` (ends with DONE). Collect: `ssh aac7 'cat ~/sess1011c/o5/G/summary.txt'`.
- Next: fill section 2 from the two summaries; if a gate or CHECK fails, the first suspects are the quarter offsets
  (`X + r m + h + hq`, `pl[3][d] + r h + hq`) and the merge's table indices (`wmi` at 2 j, j, j + hq; `wni` at q < m); then the
  e9 both-bases gate and the 4 x 10^10 gate before any merge; then remove the aac7 worktree and the aac6 clone.
