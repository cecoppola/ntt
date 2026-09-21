# L — the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` (Phase 11, PLAN.md §26 row L; 2026-09-20)

Branch `l11` (from `main` @ 72aa2e9). Files owned: `ecalc/rns_dist.c`, `ecalc/mdb.h`, `ecalc/tests/t_mn_grid.c`.
Touched outside, minimal and commented "Phase 11 L (agent L)": `ecalc/newton_db.c` — `mdb_shift` and
`mdb_to_host_all` only (the two consumers named in the task; `recip_mn` / `newton_mn_divmod` untouched);
`ecalc/dbig.c` — three lines in `sparse_get` (the spill decode for unequal row parts) and its comment.
Clone `~/ntt-l` on aac6 (built from the bundle); batches `~/l11/l_batch{1,2}.sh`, logs `~/l11/out{1,2}/`.

## C3-576 — the map

**Before.** A group of g nodes ran the transform on its first gt = 2^⌊log₂ g⌋ nodes (rank ρ = gt d + r, rows = R / 4 gt
each); the other nodes redistributed and idled in the product. At 576 = 9 · 2⁶ the top level would have idled 64 of
576 nodes (and a 3-way step 1 of 3).

**Now.** Every node of the group transforms: nr = 4 g ranks, rank ρ = g d + r (APU d of the group's node r, the layered
communicator over `G->all[d]` — the mesh over the whole group) holds

* the rows [R ρ / nr, R (ρ+1) / nr) of the R × C plane — ⌊R/nr⌋ or ⌈R/nr⌉ rows (`part0` / `partn` in rns_dist.c) — and
* the columns [C ρ / nr, C (ρ+1) / nr) of the column layout.

Its row sequence is t = j · rows + il ↔ limb m = R j + row0 + il, increasing in t, so a node's contiguous share still
meets it in one segment (`seq_start(m, R, nr, ρ)` takes the rank's actual range). For g a power of two the parts are
exactly R / nr and the transform is `ntt_dist.c`'s pipelined one on the same layered communicator: sizes 1, 2, 4 run
the same code as before, bit for bit. For any other g the transform is the general four-step in rns_dist.c
(`gen_fwd` / `gen_inv_pw`, kernels `k_twpack_g`, `k_unpack_g`, `k_pack_cols_g`, `k_unpacktw_g`; the twiddle tables are
the dist_plan's): per chunk k of the rank's rows (K = `DIST_CHUNKS`, 4), the row pass, twiddle and pack into slabs
(slab σ = my chunk rows × σ's columns, column-major, at offset col0(σ) · rk — back to back in rank order, no table), one
`comm_alltoallv` over the layered communicator with the exact per-pair counts (rows(ρ) chunk × cols(σ)), the wire time of
chunk k under the row pass and pack of chunk k + 1; then the unpacks (a table T[k][ρ] = (offset, first row, rows) of every
rank's chunk on the "my columns" side, `gplan_build`) and the column pass; the inverse mirrors it with the pointwise
product fused into the column inverse (`ntt_inv_pw_y`, as A6). The values are the same integers mod p as the one-rank
engine's by construction (an exact NTT), so the digits are identical to size 1 (measured below). `DIST_GEN=1` forces the
general map at a power-of-two g (tested at 2 and 4: same digits).

**What else had to become general.** The redistribution kernels (`k_pack_mn`, `k_scatter_mn`) take (R, nr, ρ) instead of
(rows, gt); the CRT stripes are the rank's rows × C = its sequence length q_s (the plane allocation is q = max over
ranks of rows × C and cols × R, the transform cache keys on that q — the same on every node); the spills of rank ρ land
at limb R j + R (ρ+1) / nr — `dbig.c`'s `sparse_get` decodes that when rows · nr ≠ R (the only change outside my files
that is not a consumer: three lines, the old formula kept verbatim for the equal case). The cap (`mn_logn_cap`) is
2^(min(31, POOL_LOG) + ⌊log₂ g⌋) as before, lowered by one where the rounding of R / nr would not fit the pools (never for
g ≤ 2304: at 576 nodes and 2⁴⁰ points, R = C = 2²⁰, rows 455/456, the largest rank plane is 456 · 2²⁰ = 478 M limbs of
the 2²⁹ available; at g = 9 and POOL_LOG 27, 911 · 2¹⁵ of 2²⁵). The minimum plane keeps ≥ 32 rows per rank
(logR ≥ 5 + ⌈log₂ nr⌉ — identical to the old 7 + log₂ gt at a power of two).

`mn_group.gt` / `tr[d]` are no longer read by the product (mn.c still creates `tr`; S can drop it with the communicator
rewrite). A group of one node is still not a product argument (unchanged).

## The schedule: `mn_groups_parse` (`mdb.h`, for S's `mn.c`)

`int mn_groups_parse(int size, int *out, int max)` — a pure function of (size, `MN_GROUPS`): `out[l−1]` = the group size
of tree level l ≥ 1 (the nodes [k G_l, min((k+1) G_l, size)), k = rank / G_l); returns the level count (0 at size 1).
The list must be increasing, each size a multiple of the previous or the size itself (the top group is cut by the size);
anything ≥ size ends the list at size; an invalid list aborts with the reason. Default: the powers of two up to the
largest ≤ size, then size (576 → 2, 4, …, 512, 576; 9 → 2, 4, 8, 9 — what the tree does today). `MN_GROUPS=2,4,8,16,32,64,576`
gives the 9-way top step; `…,64,192,576` a 3-way then a 3-way. A level whose ratio to the previous is k > 2 is a k-way
step: the tree combines its k children in k − 1 products over the level's group (each balanced over all its nodes, the
operands sharded over the children's subgroups — exactly what `rns_mul_dist_mn` accepts today: A over a subgroup, B and
X over another, C over the whole group); that combine loop is S's `tree_level`. The layout itself has no notion of levels.

## B7 — the four consumers on `comm_alltoallv` / `comm_allgather`

| consumer | before | now |
|---|---|---|
| the operand redistribution (`redistribute`, per operand per APU thread) | one all-to-all of g padded slabs S = (⌈maxshare/R⌉ + 1) rows; `k_gather_mn` found each point's segment by a binary search over g | `seg_tables`: my part's segment in every rank's sequence (send counts), every node's part in mine (receive counts), offsets back to back; one `alltoallv`; the received buffer **is** my sequence [0, seq_start(len)): `k_gather_mn` reads `rb[t]` (zero beyond) |
| the result exchange | `k_pack_out_mn` cut the CRT output into g padded slabs, all-to-all, `k_scatter_mn` at r · S | the CRT output `xb` is the send buffer itself (the nodes' windows are contiguous segments of it: sdsp = t0 · 8); the receive at prefix offsets, `k_scatter_mn` reads `rb[off[r] + k]` |
| the spills | an all-to-all of g copies of my 4 C limbs (send buffer g × 4 C per APU) | `comm_allgather(G->all[d], spill, spill_rb, 4 C · 8)` |
| `mdb_add_shifted` | per round of 2²⁶ limbs: sb, rb = g × 2²⁶ × 8 B per APU each | the rounds kept; both sides' cuts computed for every round first, one `alltoallv` per round with the exact cuts (`struct rng` gained `off`); sb = the largest round's send total (my part of X cut to the receivers' chunks — about two chunks), rb = one chunk: independent of g |
| `mdb_shift` (newton_db.c) | sb, rb = g × SL × 8 B per APU, SL = ⌈maxshare/4⌉ + 1, whatever the pieces' lengths | `struct piece` gained `off`; both sides' pieces from `piece_of` with prefix offsets, one `alltoallv`; sb, rb = my pieces' limbs (≤ my share / 4 per APU) |
| `mdb_to_host_all` (newton_db.c) | an all-to-all of g copies of the block (2 g × ms × 8 B on APU 0) | `comm_allgather` of one copy (g + 1 blocks) |

The redistribution's padding was only two rows per pair (C.md), so its gain is the layout (no search, no pack of the
result); the memory is in `mdb_shift` (−2 (g − 1) × share/4 × 8 B per APU per call: 17.6 GB per node at 4 × 10¹⁰ over 2
nodes, and at 576 nodes the padded form would have been g × share/4 — impossible) and `mdb_add_shifted`
(−(2 g − 3) × 512 MB per APU).

### The scratch at size 2 (`mem_report`, block pool peak live per process; 10⁹ digits, POOL_LOG=29, one node)

`mnrun.sh 2 env POOL_LOG=29 ECALC_VERBOSE=1 MEM_REPORT_DEVS=1 ./ecalc 1000000000` with `main`'s binary (`~/ntt/ecalc`, 064da26)
and with this branch, node 0's summary (GB, all four APUs of the process; `pl:peak` = the block pool's peak live bytes,
where every slab, table and temporary of the multi-node product lives; `pl:hip` = the pool's hipMalloc fallback):

| phase boundary | main: pl:peak | main: pl:hip | L: pl:peak | L: pl:hip |
|---|---|---|---|---|
| after the tree (levels over 2 nodes) | 1.9 | 0.5 | **1.5** | 0.4 |
| after bs + dm (the division's shifts and products) | 3.0 | 1.6 | **2.5** | 1.2 |

−0.5 GB of peak scratch per process at 10⁹ over 2 nodes (shares of 2.8 × 10⁷ limbs: the padded g × SL of `mdb_shift`
was 2 × 2 × 7 × 10⁶ × 8 B = 0.22 GB per APU per buffer pair, plus `mdb_add_shifted`'s g × 2²⁶ rounds and the spill
copies). Scaled to 4 × 10¹⁰ over 2 nodes (shares of 1.1 × 10⁹ limbs) the same buffers are 4.4 GB per APU per `mdb_shift`
call = **17.6 GB per node** (C.md's figure; not measurable on one node — two node-processes at 4 × 10¹⁰ do not fit it).
Digits identical in both runs; the wall 27.9 → 26.1 s (dm 18.8 → 16.9: no padded copies, no result pack) on loopback TCP.

## Tests (aac6, one node per batch, the node-processes sharing it over loopback TCP — correctness only)

Every run: `VERIFY OK` on every node, the digits (`cat` of the part files) `cmp`-identical to `~/ntt/ecalc/ref/e_<digits>.txt`
(or `results/e_1e10.out`).

Batch 1 (job 20808, ppac-pl1-s24-26, `~/l11/l_batch1.sh`):

| test | command (from `~/ntt-l/ecalc`, `J` the job) | result |
|---|---|---|
| t_mn_grid at 2, 3, 5, 6, 9 processes (5 shapes × 2 generators: the product, + X, three low products, the low cut with/without a high cut, the held product, views, `mdb_add_shifted` at four shifts; the "subgroups" shape has A over the lower half of the nodes and B, X over the rest) | `SLURM_JOB_ID=$J ./mnrun.sh <p> ./tests/t_mn_grid 0.5 27` (0.25 at 9) | VERIFY OK, 200 checks on every node, at every p (3, 5, 6, 9: "the general map"; the plane cap 2^25 / 2^26 / 2^26 / 2^27) |
| the general map forced at a power of two | `mnrun.sh 2 env DIST_GEN=1 ./tests/t_mn_grid 0.5 27`, `mnrun.sh 4 env DIST_GEN=1 …` | VERIFY OK (200) on every node |
| ecalc 10⁸ at sizes 3, 6, 9 | `mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 <out>` | identical; 9.4 / 25.8 / 62.2 s (size 9: four levels 2, 4, 8, 9 — the last a 9-node group on the general map) |
| ecalc 10⁸ at sizes 2, 4 | the same | identical; 8.0 / 8.3 s |
| ecalc 10⁹ at size 3 | `mnrun.sh 3 env POOL_LOG=29 ./ecalc 1000000000 <out>` | identical; 24.5 s |
| ecalc 10⁹ at size 1 | `./ecalc 1000000000 <out>` | identical (VERIFY OK, 14.5 s on the shared node) |

Batch 2 (job 20813, ppac-pl1-s24-26, `~/l11/l_batch2.sh`):

| test | command | result |
|---|---|---|
| ecalc 10⁹ at sizes 2, 4 | `mnrun.sh <p> env POOL_LOG=29 ./ecalc 1000000000 <out>` | identical; 26.1 / 18.9 s |
| ecalc 10⁹ at size 2, `main`'s binary (the scratch "before") | the same from `~/ntt/ecalc` | identical; 27.9 s |
| **ecalc 10¹⁰ at size 4** | `mnrun.sh 4 env POOL_LOG=29 ./ecalc 10000000000 <out>` | **identical to `~/ntt/ecalc/results/e_1e10.out`**, all 4 nodes VERIFY OK; 99.3 s (bs 28.9, dm 61.8) |
| ecalc 10⁹ at size 3 with the grids forced (`DIST_LOGN_TEST=25`: the reciprocal's and division's products as 2 × 1, 2 × 2 grids with the low cut and the low product, over the general map, the transform cache over shares hitting) | `mnrun.sh 3 env POOL_LOG=29 DIST_LOGN_TEST=25 ./ecalc 1000000000 <out>` | identical; 26.5 s |
| ecalc 10⁹ at size 6 (extra) | `mnrun.sh 6 env POOL_LOG=28 ./ecalc 1000000000 <out>` | identical; 55.8 s |
| ecalc 10⁹ at size 9 (extra) | `mnrun.sh 9 env POOL_LOG=28 …` | **not run**: the TCP meshes failed to open (`bind: Address already in use` — `mnrun.sh`'s random port base collided with another process on the shared node; no computation started). 10⁸ at size 9 (batch 1) covers the 9-node group; not retried within the node budget |

## Open issues

* The general map's exchange is not pipelined as deeply as the power-of-two one: the layered communicator keeps one
  v-exchange pending (`comm_layered.c`: posting the next completes the previous), so a chunk's wire time overlaps only
  the next chunk's row pass and pack, not two chunks as with `inflight 2`; and its v-scratch is the communicator's own
  `hipMalloc` (≈ 3 × the chunk's slab volume per APU: at 576 nodes and 2⁴⁰ points ≈ 3 × 478 M / 4 × 8 B ≈ 2.9 GB per APU
  at K = 4), not the caller's pool memory as the equal-slab scratch is — both are S's / C's file (`comm_layered.c` /
  the SHMEM transport) to fix when the v-exchange gets a consumer on the real fabric. The pool `tmp` slab of the
  equal path (q × 8 B per APU) is not allocated on the general path.
* On aac6 the general map costs more wall than the power-of-two one only because the loopback TCP transport
  serialises: 10⁸ at size 9 takes 62 s (36 processes' threads on one node) — no fabric number exists here; the
  per-rank work is 1/g of the plane at every g, which is the point.
* `mn.c` still creates the `tr[d]` sub-meshes for the first gt nodes (unused by the product now); `mn_selftest_layered`
  and `t_dist`'s layered check still test the gt-node layered communicator. Both go with S's communicator rewrite.
* The tree's k-way combine (`tree_level` for a ratio > 2) is S's; the parser and the layout are ready for it. The
  default schedule at 576 is 2, 4, …, 512, 576 (the task's stated default); `MN_GROUPS=2,4,8,16,32,64,576` selects
  the 9-way top step. Which is cheaper is X2's model's call (the 9-way step is one level of 8 products over 576 nodes
  vs three levels of 2 products over 128–576 nodes).
* The 4 × 10¹⁰ over two real nodes was not run (the nodes were never idle together); the 17.6 GB per node figure for
  `mdb_shift` is arithmetic from the buffer sizes (C.md), the measured numbers are the size-2 10⁹ ones above.
