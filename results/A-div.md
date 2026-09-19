# A-div — M4: the distributed division, with B2/B3/C3 (Phase 9, PLAN.md §19)

Branch `adiv-m4` (from `main` @ a75474d). Files owned: `ecalc/newton_db.c`, the dm flow of
`ecalc/ecalc.c`. Touched outside (minimal, commented): `ecalc/dbig.c/.h` (two fixed-length share
ops), `ecalc/newton.h` (one declaration), `ecalc/mdb.h` (a struct tag only), `ecalc/Makefile`
(dependencies of `newton_db.o`). `rns_dist.c`/`mdb.h` interfaces unchanged (A-grid's).

## Design

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

## Tests

(see the batches below)

## Open issues

- The all-to-all buffers of `mdb_shift` are g × slab per APU (the pattern of M3's redistribution):
  fine to hundreds of nodes, an all-to-all-v would remove the padding.
- `mdb_to_host_all` replicates a small number on every host; it is only used for Q's top 2 kp + 2
  limbs and the (never observed) overshoot shrink.
- X is gathered to node 0 for the output (A-out replaces this).
- The low product over shares is the full X Q; the skip of the pieces above w and B3's skip need
  A-grid's grid over shares (parameters described above).
