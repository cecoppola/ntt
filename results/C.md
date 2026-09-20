# C — B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20)

Branch `worktree-agent-ae3865b264439c2dd` (from `main` @ 4aca721). PLAN.md §21 agent C; continues results/A-comm.md.
Files: `ecalc/comm.h`, `comm_local.c`, `comm_sim4.c`, `comm_xgmi.c`, `comm_tcp.c`, `comm_layered.c`, `comm_util.c`,
`ntt_dist.c/.h`, `tests/t_dist.c`, `tests/t_comm.c`. Nothing outside the list was touched (`mn.c` unchanged).

## B7 — the unequal all-to-all

### The op

```c
void comm_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp,
                    void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s);   /* device buffers */
void comm_alltoallv_host(comm *c, ... the same ...);                                    /* host buffers, complete on return */
size_t comm_prefix(const size_t *cnt, size_t *dsp, int n);                              /* offsets of back-to-back slabs; the total */
```

Rank r receives `scnt[r]` bytes from `sb + sdsp[r]` of every rank; they land at `rb + rdsp[src]` of the receiver, which
supplies `rcnt[src]`. Both sides compute the sizes from the same descriptors (as every caller in `rns_dist.c` /
`newton_db.c` already does for its padded slabs) and **every transport aborts on a mismatch** (`scnt[me→r] ≠ rcnt[r←me]`):
the synthetic and xGMI ones from their shared tables, TCP from an 8-byte count header sent ahead of each slab, the
layered one through its stages. Counts and offsets are bytes, any values (zero allowed; 8-byte multiples are the fast
path). Completion: the same rule as `alltoall` — may return before completion, `comm_wait` completes it; a v-exchange
and an equal-slab exchange are never in flight together (each kind completes the other first).

Per transport:

| transport | device op | host op |
|---|---|---|
| local | one `hipMemcpyAsync` on the caller's stream (wait = stream sync) | memmove |
| sim4 | the four ranks post (counts, offsets, buffers); each rank's wait copies its four slabs (count check per pair) | complete at the fourth call |
| xGMI | the receivers' tables published through the shared table + barrier; my three blocks pushed by the push kernel with **per-peer lengths** (`k_push3<T>` now takes `n[3]`; a memcpy for a block that is not 8-byte aligned); wait = stream sync + barrier | memmove between two barriers |
| TCP | my slabs staged back to back (per-peer D2H copies), the self slab copied on the device; sender threads write `count, data` per peer, the receiver thread reads each peer's header, checks it, and its slab into the staging at prefix offsets; `wait` joins and uploads each slab to `rb + rdsp[r]` | below 4 KiB per slab: write-all then read-all with headers on the caller's thread; else the threads directly into `rb + rdsp` |
| layered (ρ = g d + r) | (1) the node's send counts all-gathered over the four APU threads (4 × 4 g size_t, host) so that every stage's receive sizes are known; (2) intra xGMI alltoallv: APU d sends APU d' the g slabs bound for (d', 0..g−1) as one block (the caller's slabs in place when they lie back to back in rank order — `comm_prefix` — else copied into that order); (3) transpose to [r'][d] (4 g copies); inter alltoallv over mesh d with per-node counts; (4) at wait, the [r][d] slabs into `rb` at the caller's offsets. Scratch: the comm's own (2 × the intra total + the receive total, grown as needed) | the same stages with the host ops through malloc'd temporaries |

`comm_xgmi.c`'s push kernel is now `k_push3<T>` with per-peer lengths, and two switches came with it:
`COMM_PUSH64` (64-bit remote stores instead of 16-byte vectors — RESULTS §11 had measured 909 vs 699 GB/s for the two
widths, never applied) and `COMM_PUSH_BLOCKS` (blocks per peer). **The defaults changed to 64-bit stores and 76
blocks per peer** (were 16-byte, 228) on the measurements below: the 2^31 convolution over four APUs is 8–10 % faster.

### Where G would use it (`rns_dist.c`, `newton_db.c` — not changed here)

All three consumers already compute both sides' sizes from the descriptors, so the change is: counts from the
existing tables, `comm_prefix` for the offsets, the pack/unpack kernels indexing `off[r] + i` instead of `r * S + i`,
buffers of the totals instead of `g * S`.

1. **`mdb_shift` (newton_db.c ~330–360)** — the biggest one. Today every (source, target) pair gets a slab of
   `SL = maxshare/4 + 1` limbs whatever the piece's length: `sb`, `rb` = `g × SL × 8` B per APU each. With
   ```c
   for (r) { piece_of(X, s, &Yn, n2, node, G->g0 + r, d, &hp[r]); scnt[r] = hp[r].len * 8; }   size_t ts = comm_prefix(scnt, sdsp, g);
   for (r) { struct piece q; piece_of(X, s, &Yn, n2, G->g0 + r, node, d, &q); rcnt[r] = q.len * 8; } size_t tr = comm_prefix(rcnt, rdsp, g);
   sb = db_pool_alloc(d, ts + 8); rb = db_pool_alloc(d, tr + 8);      /* hp[r].off = sdsp[r] / 8 for k_mn_pack, rdsp[r] / 8 for k_mn_scatter */
   comm_alltoallv(G->all[d], sb, scnt, sdsp, rb, rcnt, rdsp, st); comm_wait(G->all[d]);
   ```
   the buffers are the pieces' limbs: ≤ my share/4 per APU each. Saving per APU: `2 (g − 1) × maxshare/4 × 8` B —
   at 4 × 10¹⁰ over 2 nodes (shares of 1.1 × 10⁹ limbs) 4.4 GB per APU = 17.6 GB per node per call of `mdb_shift`
   (there are ~15 calls per division); at 2 048 nodes the padded form is `g × share/4`, i.e. impossible, the v form
   is `share/4`.
2. **`mdb_add_shifted` (rns_dist.c ~768–812)** — the rounds of `S = 2^26` limbs per pair: `sb`, `rb` = `g × 2^26 × 8`
   = g × 512 MB per APU each. Keep the rounds (they bound the receiver's chunk) but with the real cuts: per round
   `scnt[r] = (ht[r].b − ht[r].a) * 8` from the first `ht` loop, `rcnt[r]` from the second loop's `ht` (computed
   before the exchange), `struct rng` gains `off` (the limb offset in the slab buffer) used by `k_pack_rng` /
   `k_unpack_rng` instead of `r * S`. A sender's part of X meets the round chunks of at most ⌈|X part| / |window|⌉ + 1
   receivers (≈ 2 when X and C have similar shares), the receiver gets exactly its chunk: `rb` = 2^26 × 8 = 512 MB,
   `sb` ≈ 2 × 512 MB per APU independent of g. Saving: `(g − 1) × 2^26 × 8` B per APU for `rb` and `(g − 2) × 2^26 × 8`
   for `sb` — 0.5 GB per APU at g = 2, 1 TB (i.e. the difference between possible and not) at g = 2 048.
3. **The redistribution (`redistribute`, rns_dist.c ~470–490, and the reverse at ~625–640)** — the slab
   `S = ((maxshare − 1)/R + 2) rows` is already tight: the padding is at most two rows per pair
   (`2 g rows × 8` B ≈ 0.5 MB at g = 2, rows = 2^14), so there is no memory to save there. What the v op buys is the
   layout: with `scnt[r] = (seq_start(hi_me, R, rows, gt d + r) − seq_start(lo_me, ...)) × 8` for r < gt (0 for the
   redistribution-only nodes) and `rcnt[r] = (hseg[r].t1 − hseg[r].t0) × 8` (the `hseg` loop moved before the
   exchange), the received slabs at prefix offsets ARE the rank's sequence `[t0[0], t1[g−1])` back to back —
   `k_gather_mn`'s per-point binary search over the g segments becomes `rb[t − t0[0]]`; the same for the reverse
   (`k_pack_out_mn` at prefix offsets of `hsO`, `k_scatter_mn` reading `rb[t − hsA[0].t0]` with `hsA` computed before
   the exchange). Optional; not memory.
4. `mdb_to_host_all` (newton_db.c ~481: an all-to-all of g copies of `ms` limbs) is an all-gather: `comm_allgather(G->all[0], sb, rb, ms * 8)` with one copy of the block (g × less device memory).

In every case the whole mesh takes part (the nodes with nothing to send pass zero counts), as today.

## C4 — the two-plane pipeline (`dist_fwd2`, `dist_inv2`)

`dist_fwd2(p, x, y, s)`: the row layout in x, the column layout in y; `dist_inv2(p, x, y, s)`: the column layout in
x, the rows in y. One plane more per rank (q limbs = 4 GiB per APU at 2^31 over four ranks; the caller's). With two
planes the pipeline is the generic one for both directions: per chunk k, produce (rows + pack, or pack), post; when
k ≥ D (D = the transport's depth) wait for chunk k − D and consume it (unpack into y, or unpack-twiddle + row inverse
into y); then the tail. What changes against the one-plane schedule: forward — the unpack of chunk k runs under the
exchange of chunk k + 1 instead of all unpacks under the last exchange; inverse — chunk 0's exchange starts after
chunk 0's pack instead of after all K packs (the one-plane inverse must pack everything before the first unpack
overwrites the column layout).

The consumer would be `dist_core` (rns_dist.c, G's): per prime with A in `xa[p]`, B in `xb` and one extra plane Y
(`db_pool_alloc(r, q * 8)`): `dist_fwd2(pl, xa[p], Y); dist_fwd2(pl, xb, xa[p]); dist_pw(pl, Y, xa[p]); dist_inv2(pl, Y, xa[p])`
— the product lands in `xa[p]` as before; +q × 8 B per APU. `t_dist` exercises it with `DIST_PLANE2=1` (xGMI: the
three-plane conv per rank; TCP: one process per rank).

**Measured (xGMI, four APUs, `DIST_XGMI=1 DIST_PLANE2=1 ./tests/t_dist 31`, the conv = fwd + fwd + pw + inv at 2^31,
rank 0's wall clock; same node and batch as its baseline):**

| batch / node | one plane (baseline) | two planes | change |
|---|---|---|---|
| 1 / s24-16, 16-byte push, 228 blocks | 0.1662 / 0.1666 s | 0.1719 / 0.1707 s | **+3 %** |
| 1 / s24-16, 64-bit push, 228 blocks | 0.1584 / 0.1573 | 0.1650 | +4 % |
| 2 / s24-30, 64-bit push, 76 blocks | 0.1570 / 0.1586 | 0.1656 | +5 % |
| 3 / s24-30, the new defaults | 0.1577 / 0.1586 | 0.1678 (stats run) | +6 % |
| at 2^30 (batch 1) | 0.0848 / 0.0862 | 0.0882 / 0.0881 | +3 % |
| at 2^26 | 0.0060 | 0.0062 | — |

The two-plane schedule is **slower** by 3–6 %. `DIST_STATS` (batch 3, 2^31, three transforms per rank) says why: one
plane — rows 0.0624, cols 0.0429, packs 0.0385, exchange 0.0571, total 0.1527 s, exposed exchange 0.0089 (84 % hidden);
two planes — rows 0.0594, cols 0.0434, packs 0.0411, exchange 0.0615, total 0.1608, exposed 0.0168 (73 % hidden).
With one plane the exchange is already almost entirely hidden (the unpacks of chunks 0..K−2 fit under the last
exchange, the row passes under the earlier ones); moving the unpacks earlier puts them on the CUs at the same time as
the push kernel and the row pass of the next chunk, so the exchange itself slows down (0.057 → 0.062 s per three
transforms) and more of it is exposed. The remaining exposed part with one plane (0.009 s of 0.153 per three
transforms, 6 %) is the first chunk's row pass + pack and the last chunk's unpack, which no plane count removes.

**Not adopted**: `dist_fwd2` / `dist_inv2` stay in `ntt_dist.c` (tested in every mode by `DIST_PLANE2=1`, values
identical) but nothing uses them; the memory they would cost (q × 8 B per APU per product: 4 GiB per APU, 16 GB per
node at 2^31) stays free. Should a transport with a much faster exchange than xGMI appear (the RDMA fabric will be
slower, not faster), the measurement is a one-line switch in `t_dist`.

### What did win instead: the push kernel (`comm_xgmi.c`)

Measured on the way, because the pipeline's ceiling is the CU contention between the push kernel and the row NTT
(A-comm's finding). `COMM_PUSH64` (64-bit stores) and `COMM_PUSH_BLOCKS` (blocks per peer), 2^31 conv over four
APUs, K = 4 unless noted:

| batch / node | 16 B, 228 (old default) | 64-bit, 228 | 64-bit, 456 | 64-bit, 152 | 64-bit, 76 | 64-bit, 38 | 64-bit, 19 | 16 B, 76 |
|---|---|---|---|---|---|---|---|---|
| 1 / s24-16 | 0.1662 / 0.1666 | 0.1584 / 0.1573 | 0.1657 | | 0.1529 | | | |
| 2 / s24-30 | 0.1745 | | | 0.1641 | 0.1570 / 0.1586 | 0.1541 / 0.1565 | 0.1545 | 0.1663 |
| 2, K = 8 | | | | | 0.1542 | | | |
| 3 / s24-30 (new defaults vs old) | 0.1737 | | | | **0.1577 / 0.1586** | | | |
| 3, K = 1 (no overlap: the raw exchange) | 0.1760 | 0.1685 | | | **0.1688** | | | |
| 2^30, batch 1 / 2 / 3 | 0.0848 / 0.0883 / 0.0889 | 0.0801 | | 0.0815 | 0.0777 / 0.0814 / 0.0801 | 0.0810 | 0.0793 | 0.0862 |

64-bit stores: −5 % (RESULTS §11's 909 vs 699 GB/s, never applied to the transform's kernel). Fewer blocks: another
−3…−4 % at K = 4 (76 → 38 → 19 are within noise of each other) because the push kernel's blocks stop crowding the row
pass out of the CUs; at K = 1 (nothing to overlap) 76 blocks move the data as fast as 228 (0.1688 vs 0.1685), so the
link, not the block count, bounds a lone push. **New defaults: 64-bit stores, 76 blocks per peer** (the conservative
end of the flat range): the 2^31 convolution **0.1737 → 0.1577 s (−9 %)**, 2^30 0.0889 → 0.0801 (−10 %), 2^26
unchanged (0.0060). The same kernel serves the all-gathers and the layered communicator's intra stage. Every
`t_dist` mode and the ecalc gates below ran with the new defaults (batch 3).

## C5 — `DIST_STATS` under pipelining

The parts are timed by events on the stream they run on: begin/end pairs on the compute stream around each row pass,
pack, unpack and column pass, and on the transfer stream around each exchange (the push kernel; for a staged
transport its D2H copy); `st_flush` at the end of a transform synchronises the events and sums the pairs (a
diagnostic mode: the sync costs nothing that matters). `dist_stats` gained `t_xfer` (the exchanges' device time) and
`t_total` (first event to last per transform); `t_a2a` stays the host time blocked in post + wait = the exposed
exchange, so `1 − t_a2a / t_xfer` is the hidden fraction. `t_dist`'s line now reads
`rows … cols … pack … exchange … (exposed …: NN % hidden) transform total …`; `dist_core`'s "ntt parts" line
(rns_dist.c, unchanged) gets real rows/pack numbers at every K instead of zeros. The accumulation into the global
struct is under a mutex (the four rank threads used to race on the `+=`).

## Tests

Clone `~/ntt-c` on aac6 from the branch bundle; batches `~/c10/c_batch{1,2,3}.sh`, logs `~/c10/out{1,2,3}/`; jobs
20767 (node s24-16), 20782 and 20788 (s24-30), one node each, 45 min. Digits `cmp`-identical to `~/ntt/ecalc/ref/`
(the multi-process runs' part files concatenated).

| test | command | batch | result |
|---|---|---|---|
| TCP host-only communicator, 2/3/4/8 ranks: all-to-all, barrier, reductions, all-gathers, **alltoallv** (6 rounds: units of 8 B, 1000 B, 3 MiB, 0..8 units per pair, zero pairs, receive slabs in reverse rank order; device op and host op) | `./tests/t_comm N port` (locally, gcc), `t_comm 4 27400`, `3 27500` on the node | 1, 3 | VERIFY OK |
| synthetic four ranks + local: `alltoallv` over sim4 (lock-step, device and host), over `comm_local`, the convolutions to 2^24 | `./tests/t_dist 24`, `DIST_TINV=1 ./tests/t_dist 24` | 1, 3 | VERIFY OK (55 checks) |
| four APUs over xGMI: `alltoallv` (6 rounds, send slabs out of rank order on odd rounds) + the convolutions to 2^26 / 2^31 | `DIST_XGMI=1 ./tests/t_dist 26`, `… 31`, with `DIST_CHUNKS=1`, `COMM_PUSH64=0/1`, `COMM_PUSH_BLOCKS=19..456`, `DIST_PLANE2=1`, `DIST_STATS=1` | 1, 2, 3 | VERIFY OK (65 / 67 checks) in every run (31 runs at 2^31) |
| one process per rank over TCP, 2 and 4 ranks: tcp `alltoallv` + the convolutions; the two-plane forms over TCP | `mnrun.sh 2 ./tests/t_dist 24`, `mnrun.sh 4 …`, `mnrun.sh 2 env DIST_PLANE2=1 ./tests/t_dist 24` | 1, 3 | VERIFY OK (54) on every rank |
| the layered communicator at 2, 3, 4 node-processes: layered `alltoallv` (4 gt ranks, device and host) + the mesh's + the all-gathers + the convolutions to 2^26 | `mnrun.sh P env DIST_LAYERED=1 ./tests/t_dist 26` | 1, 3 | VERIFY OK (13 checks) on every process |
| ecalc 10⁸ at sizes 2 and 4 (one node) | `mnrun.sh P env POOL_LOG=27 ./ecalc 100000000 out` | 1 (old push), 2, 3 (new defaults) | VERIFY OK, identical |
| ecalc 10⁹ at size 1 | `POOL_LOG=29 ./ecalc 1000000000 out` | 1, 2, 3 | VERIFY OK, identical |

Not run: 2 real nodes (`mnrun.sh 2/4 env DIST_LAYERED=1 ./tests/t_dist 26` and `mnrun.sh 2 env POOL_LOG=27 ./ecalc
100000000` on `-N2`) — the other nodes were allocated by the other agents and the integrator throughout (one node
held an external `ollama-server` job for the whole session). What is new on the wire is the TCP alltoallv's count
headers and per-peer staging, which the loopback runs exercise identically.

## Gate status

| item | status |
|---|---|
| t_comm and t_dist green in every mode (sim4, local, xGMI, TCP 2/4, layered 2/3/4 node-processes) | OK |
| 2 real nodes | not run (nodes never idle); the commands are above |
| xGMI 2^31 not slower | OK: 9 % faster (0.1737 → 0.1577 s conv; fwd 0.0573 → 0.0520) from the push kernel's defaults; the transform's values identical |
| 10⁸ at sizes 2 and 4 identical | OK (three batches, incl. the new push defaults) |
| size-1 10⁹ identical | OK (three batches) |

## Open issues

- The `alltoallv` uses in `rns_dist.c` / `newton_db.c` are G's to make (the exact replacements are in the B7 section);
  until then the padded exchanges stay. `mdb_shift` is the one that matters at 4 × 10¹⁰ (17.6 GB per node per call).
- The layered `alltoallv` has no consumer yet (the tree's exchanges run over the plain meshes `G->all[d]`); its
  scratch is its own `hipMalloc` (2 × the intra total + the receive total), not the caller's slab buffer as the
  equal-slab exchange's is — fine for tests, to be pointed at pool memory when a consumer appears.
- `dist_fwd2` / `dist_inv2` (C4) are kept as measured, unused code: delete or keep as evidence with the E1 set.
- The push kernel's block count: 19–76 per peer are equally good at K = 4 on this node; 76 chosen so that a lone
  exchange (K = 1, the all-gathers, the layered intra stage) keeps its bandwidth. Not measured under the batch tier's
  concurrent transforms (`rns_mul.c` does not use `comm_xgmi`).
- `DIST_STATS` in `dist_core`'s "ntt parts" line (rns_dist.c, G's file) now prints real rows/pack numbers at any K;
  its "all-to-all" column is the host-blocked time, an upper bound of the exposed exchange (it includes the packs the
  exchanges wait for) — `t_dist`'s line prints the exposed exchange as the compute stream's idle time instead.
