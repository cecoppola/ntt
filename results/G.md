# G — grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10, PLAN.md §21)

Branch `g10` (from `main` @ 4aca721). Files owned: `ecalc/rns_dist.c`, `ecalc/mdb.h`, `ecalc/newton_db.c`, the dm flow of
`ecalc/ecalc.c` (unchanged in the end: the flow's calls did not move). Touched outside, minimal and commented "Phase 10 A6 (agent
G)" / "(G)": `ecalc/ntt_dist.c` (one static helper and `dist_inv_pw`, 8 lines; C's file), `ecalc/ntt_dist.h` (one declaration),
`ecalc/rns_mul.h` (three declarations next to the existing `rns_mul_dist_db` ones), `ecalc/tests/t_mn_grid.c` (new cases; T's
directory, A-grid's test).

## A5 — the cuts of the grid over shares (`rns_mul_dist_mn_cut`)

`rns_mul_dist_mn_cut(C, A, B, G, lowcut, highcut)` in `rns_dist.c` / `mdb.h`, exactly A-div's interface note:

* **lowcut** — a piece whose limbs end at or below the cut (`oa + ob + len_a + len_b <= lowcut`, the normalised view lengths as
  `mul_high_db` used on one node) is skipped: the A_h μ product of which only `t >> (k + 1)` is used (each skipped piece is
  < B^cut, X low by at most their number + 1, absorbed by the up-corrections). If (0,0) is skipped the first formed piece takes
  the accumulating path into the zero-filled shares (nothing else changes: `direct` was only a shortcut).
* **highcut** — pieces starting at or above it are skipped (A-grid's `w`), and the result is now **delivered in basis
  `highcut`** (`C->N = w`): the piece windows clip to the shares, and the carry out of the top node is dropped (mod B^w) instead of
  aborting (`g_top_ok` around `node_carry_in`). Before, the truncated product came out in basis na + nb with zeros above w, and
  the sharded division re-sharded it into basis w with an `mdb_shift` (an all-to-all of the whole low half); that shift is gone.
  `rns_mul_low_mn` and `rns_mul_dist_mn_v(w)` inherit the basis-w delivery (t_mn_grid's checks read the basis from C).
* `newton_mn_divmod`: `mn_prod_cut(&t, &Ah, &mu, G, k + 1, -1)` and `mn_prod_cut(&xql, &Xn, Q, G, 0, w)` at A-div's two marked
  places; `NEWTON_HIGHPROD=0` / `NEWTON_LOWPROD=0` keep the full products (the same switches as on one node).
* `rns_mul_dist_mn_shape(na, nb, G, &ka, &kb)` exposes the grid the product forms (the test's reference).

**t_mn_grid** (A-grid's test, extended): per shape and generator, the low cut at (na + nb)/2, nb/2 + 1 and na + nb (everything
skipped), each with and without a high cut at r.n − 1 — the reference is the host product minus exactly the pieces the grid
skips (from `rns_mul_dist_mn_shape` and the normalised piece lengths), then truncated; the low products check `C.N <= w`;
and (A1) a held product followed by the low product over the same B pieces. 180 checks per node at 2 processes (was 120).

## A1 — the transform cache (`rns_dist.c`; PLAN §20 I4, A-div's "B2")

A-div's design was a cache of Q's piece transforms kept from the reciprocal's last doubling to the division's X Q (48 GiB per
APU at 4e10). Building the mechanism generally showed a better use of the same planes: **within one grid product**, every
piece transform is currently recomputed per piece product (a ka × kb grid does 2 ka kb forward transforms), while the loop (j
outer over B's pieces, i inner over A's) reuses B piece j across all i and A piece i across all j. With a cache of N slots the
grid costs ≈ ka + kb transforms: A's pieces get min(ka, N − 1) slots, B's piece of the current j the rest.

Mechanism (single node and over shares, the same slot table):

* a slot = the forward transform of one operand view (key: its dbig's quarters, offset and length — over shares the view's mdb,
  offset and length, the same decision on every node of the group) on EC_NP planes of q limbs per rank, allocated from the block
  pool at first use (`db_pool_alloc`, so they survive the pool's frees and go back to it);
* `dist_core` / `mn_core` take the two designated slots; a hit anywhere skips the operand's gather and forward transform (over
  shares also its all-to-all redistribution) — B's cached planes are read in place by the pointwise product, A's are copied into
  the product plane (one device-to-device copy per prime); a miss forwards into the designated slot;
* `mul_grid` / `mn_grid` plan the slots from the grid and return the planes to the pool at the end of every product (the keys
  are addresses: no entry outlives its operands), except pinned slots: `rns_dist_cache_hold(1)` makes the next product pin its
  B pieces until `hold(0)` — A-div's B2 exactly (the reciprocal's top step uses Q as the B operand, the division's X Q hits).
  Holding is **off** unless `RNS_DIST_CACHE_HOLD=1`: the in-product policy already transforms each of Q's pieces once inside X Q,
  so the hold only moves those transforms into the reciprocal and takes the pinned slots away from the A_h μ product in between
  (at 4e10 and N = 4: 4 + 8 + 4 transforms held vs 4 + 5 + 5 unheld for the top step, A_h μ and X Q).
* `RNS_DIST_CACHE` = slots (default 3; 0 = off), 16 GiB per slot per APU at 2^31 points (4 GiB over 2 nodes).
* `mul_high_db` (A-div's B3, in newton_db.c) became `mul_grid`'s `lowcut` (`rns_mul_high_db`), so the cache serves it too; the
  same split and piece order, bit for bit.
* `recip_mn` uses Q itself at the top step (Q_t = Q: no shift copy — one all-to-all less per top step).

Expected at 4e10 (2^31 planes, one forward transform + gather ≈ 0.36 s): the reciprocal's top step 6 → 4, r d 4 → 3, A_h μ
10 → 5, X Q 10 → 5, the top tree levels (5 × 1 and 1 × 2 grids) −5: ≈ −15 transforms ≈ −5 s, if the block pool holds the planes
without falling back to `hipMalloc` (0.057 s/GB, serialised under the pool's mutex).

## A6 — the distributed tier's local transforms

* The pointwise product is fused into the column inverse's first pass: `dist_inv_pw(p, x, y, s)` in `ntt_dist.c` (=
  `dist_pw` + `dist_inv` through `ntt_inv_pw_y(FULL)`, the same modmul on the same canonical operands: bit-identical), used by both
  tiers in `rns_dist.c` (`DIST_PW_FUSE=0` restores the two passes). One plane read + write per prime per product less.
* Body 1 (N-kernel's register-blocked kernel) runs only for 7-stage passes, i.e. local lengths ≥ 2^17 (b1 takes 10 stages, a
  pass up to 7): the dist tier's rows/columns are 2^15/2^16 at a 2^31 plane, so it never reaches them. `DIST_LOGR_DELTA` moves the
  four-step split (logR = logn/2 + delta: +1 makes the rows 2^16 × columns 2^15 at 2^31 — still no 7-stage pass; the split that
  gives one, 2^14 × 2^17, needs delta −1, measured below). The radix-3 (`DIST_R3`) path keeps the separate pointwise (off by
  default).

## Tests

Clone `~/ntt-g` on aac6 (and `~/ntt-g-a1` for the size-1 batches), built from the branch bundle; scripts `~/g_b1.sh` .. `~/g_b5.sh`,
outputs and logs under `~/g10/b1..b5/`. Every run: `VERIFY OK` on every node and the digits (`cmp` of the file, or of `cat` of
the part files) byte-identical to `~/ntt/ecalc/ref/e_<digits>.txt` / `results/e_4e10.out`. One node per batch, the
node-processes sharing it (TCP over loopback: multi-process walls are correctness only).

### A5 (batch 1 / 3 / 5: jobs 20762, 20771, and the final one)

`SLURM_JOB_ID=$J ./mnrun.sh <s> env POOL_LOG=27 [BS_MDEV_LOGL=21] ./ecalc 100000000 out`, `... POOL_LOG=29 BS_MDEV_LOGL=25 ./ecalc 1000000000 out`:

| digits | sizes | extra | result |
|---|---|---|---|
| 10⁸ | 2, 3, 4 | (and size 2 with `BS_MDEV_LOGL=21`: the device top levels) | identical, all nodes VERIFY OK |
| 10⁸ | 2 | `DIST_LOGN_TEST=22` (the mn cap lowered: the division's products as 1 × 2 grids with the low cut and the low product in basis w) | identical |
| 10⁹ | 2, 4 | | identical |
| 10⁹ | 2, 3, 4 | `DIST_LOGN_TEST=25`: the A_h μ product 2 × 2 with one piece skipped by the low cut, X Q 2 × 2 in basis w | identical |
| 10⁹ | 1 | (the single-node flow, untouched by A5) | identical |

`t_mn_grid 0.5 28` at 2, 3 and 4 node-processes: VERIFY OK, 200 checks per node (was 120: the cut cases and the held product).
The sharded division's low product no longer re-shards X Q into basis w (one `mdb_shift` of the whole low half less: 51 shifts
per run instead of 52 at 10⁹).

### A1 (batches 2 and 4: jobs 20766, 20774 on s24-30 / s24-16; 4 × 10¹⁰ size 1, `RNS_VERBOSE=1 ECALC_VERBOSE=2`, the reference evicted first)

The transform cache at size 1, the planes allocated once at the first grid product (the top tree level) and freed after the
division; digits identical in every run:

| run | cache | planes' allocation | bs (mdev top levels) | recip | division | dm | wall |
|---|---|---|---|---|---|---|---|
| base (batch 2, fused pointwise off) | off | — | 38.3 (13.9) | 16.9 | 17.7 | 34.8 | 86.0 |
| fuse (batch 2) | off | — | 38.1 (13.8) | 16.6 | 17.1 | 33.8 | 87.7 (init 15.7) |
| c0 (batch 4) | off | — | 36.7 (13.2) | 16.1 | 16.3 | 32.6 | **85.6** |
| c1 | 1 slot | 3.6 s | 40.2 (16.4) | 15.1 | 15.2 | 30.4 | 86.6 |
| c2 | 2 slots | 7.4 s | 43.1 (19.6) | 15.6 | 14.2 | 30.0 | 89.6 |
| c2b (repeat) | 2 slots | 9.3 s | 45.1 (21.4) | 15.1 | 14.1 | 29.3 | 89.8 |
| dm1 (2 slots, `DIST_LOGR_DELTA=-1`) | 2 | 8.8 s | 45.4 (22.0) | 16.8 | 15.3 | 32.2 | 94.4 |
| c3 from the block pool (batch 2, the first version) | 3 | pool fallback 3 × 64 GB | 135.3 | 26.2 | 27.2 | 57.4 | 208.1 |

Per product (the `dist_db` lines): the reciprocal's top step (3 × 1 pieces) 3.95 → 3.18 s, r d (1 × 2) 2.51 → 2.14, A_h μ
(2 × 3, 5 formed) 6.89 → 5.75, X Q (2 × 3, 5 formed) 7.23 → 5.70, the two level-24 products 2.51/3.00 → 2.12/2.66, level 25's
5 × 1 grids 1.70/1.65 → 1.43/1.39: **−4.1 s of products at 2 slots** (a piece product with a B hit: 1.11 → 0.72 s, load 0.33 →
0.17, ntt 0.67 → 0.44), 20 hits of 43 operand transforms in dm. But the two 16 GiB planes per APU cost **7.4–9.3 s to map**
(hipMalloc on the four APUs in parallel, 0.055 s/GB; one slot 3.6 s) — the same cost the block pool's fallback pays, and the
pool has no room for them at the products' peaks (peak live 121.5 of 141 GB; the first version's 3 × 64 GB from the pool
oversubscribed the node: 208 s) — so the wall loses by 1–4 s. Mapping them at init would move, not remove, the cost (RESULTS
§70: the mapping stalls running kernels too). **Not adopted at size 1: `RNS_DIST_CACHE` defaults to 0** (the code is the
measured form of A-div's B2 / PLAN I4; the hold of Q's pieces across the two products, `RNS_DIST_CACHE_HOLD=1`, adds nothing
over the in-product policy, see the design note). At 10⁹ (one plane per product) the cache is not entered.

Over shares (batch 3, 10⁹ over 2 node-processes with the grids forced by `DIST_LOGN_TEST=25`, `RNS_DIST_CACHE_MN`): dm 21.4 s
(cache 0) → 19.1 s (2 slots) → 18.4 s (4 slots); the division's products 9.7 → 7.9 → 6.8 s (a hit skips the operand's
all-to-all redistribution as well). The planes over gt nodes are 1/gt the size (8 GiB per APU per slot at 4 × 10¹⁰ over two
nodes, 2 s to map). **Adopted over shares: `RNS_DIST_CACHE_MN` defaults to 2**; the group agrees on the slot count (the minimum of
what every node's free memory allows). 4 × 10¹⁰ over two real nodes was not run (the 1 GbE fabric: hours).

Size-1 correctness of the cache (batch 2/4, all identical): 10⁹ with `DIST_LOGN_TEST=25` (grids of 2 × 1 .. 3 × 4 pieces, 28 hits)
at 1, 2 and 4 slots; the hold (`RNS_DIST_CACHE_HOLD=1`); `LIMB_BASE=2`; `t_dbig 24 big` with and without the cache (batch 5).

### A6 (batch 2: `t_dist`; the 4 × 10¹⁰ runs above)

`./tests/t_dist 24`, `DIST_TINV=1 ./tests/t_dist 24`, `DIST_XGMI=1 ./tests/t_dist 31` (+ `DIST_STATS=1`): VERIFY OK (67 checks);
xGMI 2³¹: fwd 0.0573 s, fwd+fwd+pw+inv 0.1750 s (t_dist itself calls `dist_pw` + `dist_inv`: unchanged by construction).
The fused inverse in the products: a 2³¹ piece product's ntt 0.73 → 0.67 s (base → fuse, both cache off), dm 34.8 → 33.8 s
(−1.0 s; wall within the run-to-run spread). `DIST_LOGR_DELTA=-1` (rows 2¹⁷ on body 1, columns 2¹⁴): ntt 0.65 but load 0.54
(the gather's column runs are shorter) — total 1.33 vs 1.11 s per plane: worse, not adopted (the split stays logn/2).

## Open issues

* The cache at size 1 pays only if its planes are already mapped: 32 GiB per APU of mapped, idle device memory during the
  top levels and dm does not exist today (the plane pools are in use by the product, the block pool is at its peak); a plane
  pool sized for it at init (M's `rns_init`) would cost the same mapping there. If B3 (the 3·2³⁰ planes, +34 GiB per APU)
  is ever adopted, the cache could share that memory when the product is not radix-3.
* The cache over shares was measured at 10⁹ over node-processes on one node (grids forced); at 4 × 10¹⁰ over two real nodes
  the 8 GiB per APU per slot (2 s to map) against the skipped redistributions of 2^28-limb pieces is the expected win.
* `t_dbig 0 big` (the 2³¹ × 2³⁰ product with its 25 GB host reference) was killed by the host memory limit in batch 2 (as
  A-grid saw) and ran out of the job's time in batch 4; batch 5 runs `t_dbig 24 big`.
* Body 1 never reaches the dist tier's local passes (2¹⁵/2¹⁶ points: no 7-stage pass); a split with a 2¹⁷ side loses more in
  the gather than it gains. A b16 kernel variant for 6-stage passes is N-kernel's territory.
