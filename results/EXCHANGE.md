# EXCHANGE — the all-to-all exchange in every transport

Scope: the all-to-all exchange in every transport: all-gather, layered exchange two-deep, slab pipelining, the rank gradient, gather kernels. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A-comm | M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19) |
| C | B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20) |
| X13 | xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a |
| X13b | the uneven exchange two deep, one-kernel transposes, faster packs (PLAN.md §31 row X, axis A) |
| A07 | (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products |
| K1b | raw-once gather (S39 candidate A, O7b lever 3) -- IMPLEMENTED behind DIST_GATHER_RAW (off), probe + gates + ABBA ARMED o |
| S50 | k_gather coalesced write (DIST_GATHER_TILE, off by default) -- IMPLEMENTED, gated on CPX (test), timing queued (aac7 Job |
| P2 | speed up k_gather -- STOPPED after a micro-probe: the kernel is bound by the remote read, and the best possible kernel-l |
| K6 | single-node C-tier all-to-all — overlap across primes (RNS_DIST_OVERLAP=1, off by default) |

## A-comm — M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19)

(former file: A-comm.md)

Branch `worktree-agent-a69986025b3273e8b` (from `main` at a75474d). PLAN.md §19, agent A-comm;
results/GRID_GROUPS.md §M3 "Open issues" (the O(g) point-to-point all-gathers).

#### Files

| file | what |
|---|---|
| `ecalc/comm.h` | `allgather_host` op; `comm.inflight` (exchanges a caller may post before a wait); `comm_allgather_host` |
| `ecalc/comm_util.c` | `comm_allgather` / `comm_allgather_host`: the transport's op, else the day-0 fallback (no transport uses it now) |
| `ecalc/comm_local.c` | allgather = memcpy; `wait` synchronises the stream the copy was posted on (the pipeline posts on a transfer stream) |
| `ecalc/comm_sim4.c` | allgather completes when the fourth rank calls; `inflight` 0 (no pipelining: every rank posts before any waits); a device sync before the copies (the ranks' packs are on separate streams now) |
| `ecalc/comm_xgmi.c` | allgather: each rank pushes its block into the three peers' receive buffers with the push kernel (a memcpy for non-16-byte sizes), two barriers; host variant: memcpys between two barriers |
| `ecalc/comm_tcp.c` | the receive side of the all-to-all in a thread (`alltoall` returns once the send slabs are staged); allgather (device: staged; host: below 4 KiB write-all/read-all without threads, else sender threads + receiver) |
| `ecalc/comm_layered.c` | two exchanges in flight (`inflight` 2): the xGMI stage and transpose of exchange k+1 run while exchange k is on the inter-node wire; each has a slot of the scratch (two slots of 4 g x bytes; one slot = serialised); waits counted logically; allgather = inter first (mesh d gathers my block over the nodes into slot d of the result) then the xGMI all-gather of the g-block slots — no transpose, 4 x less on the fabric than intra-first |
| `ecalc/ntt_dist.c/.h` | the K-chunk slab pipeline (`DIST_CHUNKS`, default 4) on a per-plan transfer stream; `dist_fwd_pre/_post`, `dist_inv_pre/_post` keep their meaning (pre ends with the last post) |
| `ecalc/mn.c` | `mn_allgather` → `comm_allgather_host` (the only change there) |
| `ecalc/tests/t_comm.c`, `t_dist.c` | all-gather checks (host / device / in place) in every mode; xGMI timing lines at ≥ 2^26 |

#### Design

##### The all-gathers

`allgather(c, sendbuf, recvbuf, bytes)`: every rank's block into `recvbuf[size][bytes]` in rank order,
complete on return; `sendbuf` may already be its own slot (`recvbuf + rank * bytes`, in place).
`allgather_host` is the same for host buffers — the tree's descriptors (8 u64 per node), the carry flags
(one byte per node) and the residues are host values, and staging them through the device would cost more
than the exchange. Per transport:

- local: memcpy. sim4: the fourth caller performs every rank's copies (the ranks are driven in lock step).
- xGMI: the sender pushes (`k_push3`, 64-bit... the existing 16-byte-vector push kernel) its one block into the
  three peers' slots; barrier before (the receive buffers are known) and after (everyone's block has landed).
- TCP: device blocks are staged through the pinned host buffers; host blocks up to 4 KiB go write-all then
  read-all on the caller's thread (never fills a socket buffer), larger ones with one sender thread per peer
  and the receiver thread.
- layered (4 g ranks, ρ = g d + r): `inter` first — mesh d all-gathers my block over the g nodes straight
  into slot d of the result (`recvbuf + d g bytes`, blocks [d][r]); then the xGMI all-gather of the four
  g-block slots (mine is in place). Every mesh carries one block per node: 4 (g−1) bytes per node on the
  fabric, against 16 (g−1) for intra-first (each mesh would carry four blocks), and no transpose.

What replaces the O(g) loops: `mn_allgather` (mn.c) now is `comm_allgather_host`. In `rns_dist.c`
(A-grid's file, untouched): `node_carry_in`'s write-all/read-all of one byte is
`comm_allgather_host(G->all[0], &v, all, 1)`, and the spill all-gather (g copies of the 4 C-limb spill into
`spill_sb` + an all-to-all) is `comm_allgather(G->all[d], v->spill, spill_rb[d], C * 4 * 8)` — g x less
device memory for `spill_sb` and no copies.

##### The slab pipeline (`ntt_dist.c`)

The rank's `rows` rows are cut into K chunks of `rows/K` (≥ 32, a power of two; K = min(DIST_CHUNKS, rows/32),
1 over sim4). The slab buffers are partitioned into K chunk regions `[k][size][cols][rows/K]`, so chunk k's
exchange is an ordinary all-to-all of `cols · rows/K` limbs per slab, posted on the plan's transfer stream
`ts` after an event recorded on the compute stream `s` behind the pack; after the wait an event on `ts`
orders the unpack on `s` behind the transfer. The kernels take a row offset (twiddle rows are global rows).

- Forward: `prod(k)` = the length-C row pass of chunk k's rows + `k_twpack` into region k. Schedule:
  prod(0), post(0); for k ≥ 1: prod(k), [wait(k−1) when one may be in flight], post(k); then the unpacks of
  chunks 0..K−2 (which write the column layout over all of x, so they must follow every row pass — stream
  order guarantees it since all prods are enqueued first); `_post`: wait(K−1), unpack(K−1), the column pass.
  So the row pass + pack of chunk k+1 runs under the exchange of chunk k, and the unpacks under the last one.
- Inverse: the column pass, then every chunk packed (`k_pack_cols` reads all of x), D = min(inflight, K)
  chunks posted; loop: wait(k), `k_unpacktw` + the row inverse of chunk k (contiguous rows: in place under the
  exchange of chunk k+1), post(k+D). `_pre` ends with the last post.
- Over the layered communicator (`inflight` 2) two chunks are on the wire: chunk k+1's xGMI stage and
  transposes run while chunk k is on TCP (the TCP transport takes one exchange at a time, so posting k+1
  completes k before its own inter stage — the caller's k-th wait is then a logical no-op).
- `DIST_STATS` in the chunked mode: `t_a2a` is the *exposed* exchange time (host time blocked in post + wait);
  the per-part times are measured only with K = 1 (they need stream syncs).

The numerics are untouched: a row's transform does not depend on its batch, the packs are permutations.

##### mnrun.sh

Unchanged; no launching problem was reported by the other agents during this session (their batches ran
`mnrun.sh` with 1–4 node-processes on one node alongside mine).

#### Tests

Build in `~/ntt-acomm` on aac6 from the branch bundle. Batch 1: job 20710 (one node, s24-26); batch 2: job
(below). Every ecalc digit file `cmp`-identical to `ref/e_<digits>.txt`.

##### Batch 1 (job 20710, one node)

| test | command | result |
|---|---|---|
| TCP communicator, host-only, 4 ranks (all-to-all, barrier, reductions, all-gathers host/device/in place at 1 B … 3 MiB) | `./tests/t_comm 4 27400` (locally also 2, 3, 8 ranks) | VERIFY OK |
| synthetic four ranks | `./tests/t_dist 24` | FAILED 1 of 53 (prime 0, 10x10: every point) — the M7 transfer stream is non-blocking and no longer ordered behind the other ranks' packs on the null stream; fixed (a device sync in `s_wait`), re-run in batch 2 |
| four APUs over xGMI, 4 chunks | `DIST_XGMI=1 ./tests/t_dist 26` | VERIFY OK (65 checks, incl. the all-gathers) |
| four APUs, one exchange | `DIST_XGMI=1 DIST_CHUNKS=1 ./tests/t_dist 26` | VERIFY OK (65) |
| four APUs, transposed inverse | `DIST_XGMI=1 DIST_TINV=1 ./tests/t_dist 26` | VERIFY OK (65) |
| synthetic, transposed inverse | `DIST_TINV=1 ./tests/t_dist 26` | VERIFY OK (66) |
| one process per rank over TCP, 2 and 4 ranks | `mnrun.sh 2 ./tests/t_dist 24`, `mnrun.sh 4 …` | VERIFY OK (53) on every rank (incl. the TCP all-gathers) |
| the layered communicator, 2, 3, 4 node-processes | `mnrun.sh P env DIST_LAYERED=1 ./tests/t_dist 26` | VERIFY OK (12 checks: 10 convolutions to 2^26, the layered all-gathers over 4 gt ranks, the mesh all-gathers) on every process |
| ecalc 10⁸, sizes 2 and 4 (one node) | `mnrun.sh P env POOL_LOG=27 ./ecalc 100000000 out` | VERIFY OK, identical; 5.7 s / 6.2 s wall (tree levels 1.9 / 2.2 s) |
| ecalc 10⁹, size 1 | `POOL_LOG=29 ./ecalc 1000000000 out` | VERIFY OK, identical (8.27 s wall) |

(`t_dist` in the COMM_RANK mode at 3 ranks is not a valid configuration — the rank's rows must be a power
of two ≥ 32; it never was. The layered mode covers 3 node-processes.)

##### Batch 2 (job 20728, one node s24-30; the other two nodes were allocated throughout, so no real-node runs)

| test | command | result |
|---|---|---|
| synthetic four ranks, with the `s_wait` fix | `./tests/t_dist 26`, `DIST_TINV=1 ./tests/t_dist 26` | VERIFY OK (66 checks) both |
| TCP communicator, 3 and 8 ranks | `./tests/t_comm 3 27500; ./tests/t_comm 8 27600` | VERIFY OK both |
| four APUs to 2^31, K = 1 / 4 / 8 / 4 | `DIST_XGMI=1 DIST_CHUNKS=K ./tests/t_dist 31` | VERIFY OK (67 checks) each; timings below |

##### The xGMI pipeline at 2^30 points (four APUs, `DIST_XGMI=1 ./tests/t_dist 31`, job 20710)

One transform per rank of 2^30 / 4 points, prime 1, R = C = 2^15 (rows = 8192 per rank). Times are rank 0's
wall clock of one forward, and of the whole convolution (two forwards, the pointwise product, one inverse):

| DIST_CHUNKS | fwd | fwd+fwd+pw+inv | note |
|---|---|---|---|
| 1 (the old schedule) | 0.0296 s | 0.0883 s | DIST_STATS: rows 0.0223, cols 0.0215, pack 0.0150, exchange 0.0264 (three transforms, per rank) |
| 2 | 0.0290 | 0.0865 | |
| 4 (default) | 0.0257 | 0.0811 / 0.0806 | −13 % / −8 % |
| 8 | 0.0249 | 0.0784 | −16 % / −11 % |

At 2^31 points (batch 2, R = 2^16, C = 2^15, rows = 16384 per rank; the same node's second run of each):

| DIST_CHUNKS | fwd 2^30 | conv 2^30 | fwd 2^31 | conv 2^31 | stats (K = 1, 2^31, three transforms) |
|---|---|---|---|---|---|
| 1 | 0.0315 / 0.0303 | 0.0929 / 0.0931 | 0.0597 / 0.0595 | 0.1792 / 0.1771 | rows 0.0430, cols 0.0427, pack 0.0320, exchange 0.0536 |
| 4 | 0.0285 / 0.0280 | 0.0873 / 0.0877 | 0.0578 / 0.0569 | 0.1737 / 0.1734 | −4 % / −2.5 % at 2^31 |
| 8 | 0.0273 | 0.0849 | 0.0567 | 0.1704 | −5 % / −4 % at 2^31 |

(batch 2's node ran the K = 1 case 5 % slower than batch 1's node at 2^30; compare within a batch.)
The exchange is 0.0264 s of the 0.0883 s convolution (30 %); the pipeline hides 0.0072 s of it at K = 4 and
0.0099 s at K = 8, i.e. 27–37 % of the exchange, 8–11 % of the transform. What can be hidden: in the forward
only the row pass and pack of the next chunk (0.012 s per transform); in the inverse the unpack and row
inverse of the previous one; the push kernel and the NTT kernels share the CUs (a push kernel overlaps
compute at ≈ 74 %, RESULTS §11), so the ceiling is roughly half the exchange. At 2^26 the chunks cost
nothing at K ≤ 4 (0.0059 vs 0.0058–0.0063 s) and 20 % at K = 8 (0.0072–0.0077 s: launch overhead on
1-ms transforms); hence the default 4. At 2^31 the gain is smaller (2.5–4 %): with 16384 rows the push
kernel (228 x 3 blocks) and the row NTT of the next chunk contend for the CUs for longer, and the
exchange itself (0.054 s for 3 x 4 GB per rank ≈ 220 GB/s per rank, one direction) is memory-bound
against the row pass that reads and writes the same HBM. The measured overlap: **27–37 % of the exchange
hidden at 2^30, 15–25 % at 2^31** (K = 4 / 8) — the fabric stage is the part that a pipelined transform
can hide; the row pass it overlaps is the shorter part. Whole `t_dist 31` runs (dominated by the host reference): `main`'s
binary 64.7 s / 63.4 s, this branch 65.0 s — the same. The 10⁹ single-node run (whose products go through
`dist_core` over the xGMI communicator, now chunked) is identical and 8.27 s wall (M3's table: 8.9 / 7.8 s).

#### Gate status

| item | status |
|---|---|
| t_comm, t_dist in DIST_XGMI=1, COMM_RANK (2, 4) and DIST_LAYERED=1 (2, 3, 4 node-processes) modes on one node, to 2^26 | OK |
| the same on 2–3 real nodes | **not run**: the other two nodes were allocated by other agents for the whole session (batch 2 checked `sinfo` and found one idle node). The layered code paths across hosts are M3's, verified there on 2–3 real nodes; what is new on the wire is the TCP receiver thread and the second exchange in flight, both host-side logic that does not depend on the hosts being distinct. To run when idle: `mnrun.sh 2/4 env DIST_LAYERED=1 ./tests/t_dist 26` and `mnrun.sh 2 env POOL_LOG=27 ./ecalc 100000000 out` on `-N2` (the script `~/acomm_batch2.sh` does it when two nodes are idle). |
| ecalc 10⁸ at sizes 2 and 4 identical | OK (one node) |
| the single-node 10⁹ bit-for-bit | OK |
| xgmi t_dist 31 timing not slower | OK: the transform is 2.5–8 % faster at 2^30–2^31; the whole run is equal to `main`'s (65.0 vs 63.4–64.7 s, host-bound) |

#### Open issues

- `rns_dist.c` (A-grid) still has the two O(g) loops (`node_carry_in`, the spill all-gather as g copies +
  all-to-all); the one-line replacements are given above. Not changed here to avoid a merge conflict with
  A-grid's rewrite of that file.
- The `DIST_STATS` breakdown in the chunked mode: only the exposed exchange and the column pass are
  measured (the row pass and packs run under the exchange); `dist_core`'s "ntt parts" line therefore
  shows rows/pack as 0 unless `DIST_CHUNKS=1`.
- The layered communicator's two-in-flight mode needs two scratch slots (2 x 4 g x chunk bytes); the
  product tier hands it one slab buffer (q x 8), which holds two slots for K ≥ 2 — so `rns_mul_dist_mn`'s
  transforms pipeline the xGMI stage under the TCP stage automatically. Not measured (1 GbE).
- The forward pipeline cannot overlap the unpack of chunk k with the row pass of chunk k+1 (both are in
  x); the alternative — unpacking into the receive buffer's own region and running the column pass from
  there — would need the column layout in a second plane. The inverse has the same constraint in the
  packs. A ring of two extra chunk-sized planes would lift it; not done (memory).
- The sim4 harness's all-gather completes when the fourth rank calls; `mn_selftest`-style sequential
  drivers must call all four before reading (as `t_dist` does).
- `t_dist` in the COMM_RANK mode requires a power-of-two rank count (rows ≥ 32); 3 ranks is not a valid
  configuration of that mode (the layered mode handles 3 node-processes with gt = 2).

#### Files touched outside my list

`ecalc/mn.c`: `mn_allgather`'s body only (→ `comm_allgather_host`), as allowed.

## C — B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20)

(former file: C.md)

Branch `worktree-agent-ae3865b264439c2dd` (from `main` @ 4aca721). PLAN.md §21 agent C; continues results/EXCHANGE.md §A-comm.
Files: `ecalc/comm.h`, `comm_local.c`, `comm_sim4.c`, `comm_xgmi.c`, `comm_tcp.c`, `comm_layered.c`, `comm_util.c`,
`ntt_dist.c/.h`, `tests/t_dist.c`, `tests/t_comm.c`. Nothing outside the list was touched (`mn.c` unchanged).

#### B7 — the unequal all-to-all

##### The op

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

##### Where G would use it (`rns_dist.c`, `newton_db.c` — not changed here)

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

#### C4 — the two-plane pipeline (`dist_fwd2`, `dist_inv2`)

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

##### What did win instead: the push kernel (`comm_xgmi.c`)

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

#### C5 — `DIST_STATS` under pipelining

The parts are timed by events on the stream they run on: begin/end pairs on the compute stream around each row pass,
pack, unpack and column pass, and on the transfer stream around each exchange (the push kernel; for a staged
transport its D2H copy); `st_flush` at the end of a transform synchronises the events and sums the pairs (a
diagnostic mode: the sync costs nothing that matters). `dist_stats` gained `t_xfer` (the exchanges' device time) and
`t_total` (first event to last per transform); `t_a2a` stays the host time blocked in post + wait = the exposed
exchange, so `1 − t_a2a / t_xfer` is the hidden fraction. `t_dist`'s line now reads
`rows … cols … pack … exchange … (exposed …: NN % hidden) transform total …`; `dist_core`'s "ntt parts" line
(rns_dist.c, unchanged) gets real rows/pack numbers at every K instead of zeros. The accumulation into the global
struct is under a mutex (the four rank threads used to race on the `+=`).

#### Tests

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

#### Gate status

| item | status |
|---|---|
| t_comm and t_dist green in every mode (sim4, local, xGMI, TCP 2/4, layered 2/3/4 node-processes) | OK |
| 2 real nodes | not run (nodes never idle); the commands are above |
| xGMI 2^31 not slower | OK: 9 % faster (0.1737 → 0.1577 s conv; fwd 0.0573 → 0.0520) from the push kernel's defaults; the transform's values identical |
| 10⁸ at sizes 2 and 4 identical | OK (three batches, incl. the new push defaults) |
| size-1 10⁹ identical | OK (three batches) |

#### Open issues

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

## X13 — xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a

(former file: X13.md)

X: the 3-node slot was never available (admin CI array, then s24-30 down); request stands for the next session.


Branch `p13-X` (from `main` @ 203733b). Files: `ecalc/comm_layered.c`, `ecalc/comm_xgmi.c`, `ecalc/tests/t_dist.c`;
nothing else touched (`ntt_dist.c`, `comm_tcp`, `comm.h` unchanged — the stats live in the communicators). Defaults
unchanged: every addition is behind `COMM_LAYER_STATS`, `COMM_XGMI_STATS`, `DIST_H4`, `DIST_LBENCH` (all off by default).
aac6 clone `~/ntt-X13`; batches `~/x13/b*.sh`, logs `~/x13/b*/`.

Labels: **measured** = on aac6 in this session; **modelled** = arithmetic on measured constants; **assumed** = PLAN §25/§29.

#### 1. What was built

##### `COMM_LAYER_STATS=1|2` — the layered exchange's stage timeline (`comm_layered.c`)

Per exchange (equal-slab and `alltoallv`, both forms), host `CLOCK_MONOTONIC` stamps: entry, the xGMI stage
`[x0, x1]` (intra post .. intra wait return), ready-for-the-fabric `r1` (after the transpose), the fabric stage `[f0, f1]`
(inter post .. inter completion), complete `c1` (after the transpose back). One `clock_gettime` per stamp, records in a
per-communicator array; nothing when the switch is off (one predictable flag test per stage).

* `f1` in mode 1 (passive) is the return of the inter wait where the pipeline calls it (lazily) — an upper bound on the
  fabric's completion. Mode 2 (exact) starts a watcher thread per inter exchange that calls the wait at once and stamps
  its return (the TCP upload then happens as soon as the data lands; values identical, timing perturbed slightly).
* Reported at exit (and by `comm_layered_stats_report(tag)`): per APU thread (sum over the four / 4) span, xGMI-busy,
  fabric-busy, **both-busy**, xGMI-only, fabric-only, neither; both/xGMI, both/min(x,f), the saving against sequential
  both/(x+f) and its ceiling min/(x+f); the one-at-a-time measures — **held** (post − ready: how long a ready exchange
  waited for the previous inter stage) and **fabric idle while held** (the part of it in which the fabric had already
  finished); per-size buckets (fabric bytes per APU per exchange); the block-transpose time; the NIC balance (fabric
  bytes and time per APU thread); and a **node view** (the union over the four APU threads of each kind).
* With `COMM_XGMI_STATS=1` as well, each exchange also carries the **link-active interval** of its push kernel (the
  stamped kernel duration, aligned to the host's return from the stream sync), and the report gives the xGMI *stage*
  against its *link time* and **both-busy at link level** (push kernel ∩ fabric). This distinction turned out to be the
  main result (§3).

##### `COMM_XGMI_STATS=1` — H4 per-link bytes and rates (`comm_xgmi.c`)

`k_push3_ts`: the push kernel with each block's start and end stamped (`wall_clock64`, 100 MHz); per sender → peer the
bytes and the time any of that peer's blocks ran (= the link's active time), per push the kernel span. Printed at exit.
`comm_xgmi_h4()` is a benchmark entry (nine modes: the all-to-all push, contiguous block mapping, fan-out, one link with
3× or 1× blocks, one link both ways, three kernels on three streams, `hipMemcpyAsync` on three streams, three rounds of
one link each).

##### Drivers (`tests/t_dist.c`)

* `DIST_H4=1 [DIST_H4_MB=256] ./tests/t_dist` — H4 on one node (four APU threads).
* `DIST_LAYERED=1 DIST_LBENCH=24,26,28 [DIST_LREPS=3] ./mnrun.sh <g> ./tests/t_dist` — timed layered transforms of one
  plane (fwd x, fwd y, inverse with the pointwise fused) over the transform nodes, then the stage report per size.

#### 2. H4 — does the push saturate all three K4 links? (measured, job 21006, s24-30, `~/x13/b1/h4_*.txt`)

256 MiB per (sender, peer), 10 repetitions; wall = bytes sent / barrier-to-barrier time; per link from the stamps.

| form | per APU (wall) | node | per link (stamped) |
|---|---|---|---|
| one link 0→1 (all blocks on it) | 86.8 GB/s | — | 89.4 |
| one link, both directions at once | 84.6 each | 169.2 | 87.5 / 87.8 |
| fan-out: APU 0 → its 3 peers (others idle) | **253.5** | — | 91.1 / 86.0 / 91.0 |
| **all-to-all push (the exchange; default 76 blocks/peer)** | **244.0** | **976.2** | 82.7 – 89.3 |
| all-to-all, contiguous thirds of the grid per peer | 249.9 (+2.4 %) | 999.6 | 85.0 – 85.7 (balanced) |
| all-to-all, 3 kernels on 3 streams | 237.9 (−2.5 %) | 951.6 | — |
| all-to-all, `hipMemcpyAsync` on 3 streams (copy engines) | 134.8 (−45 %) | 539.3 | — |
| all-to-all in 3 rounds, one link at a time | 86.7 (= 1 link) | 346.9 | 87–90 |

Blocks per peer 19 / 38 / 76 / 152 / 304: 230.8 / 242.2 / 244.0 / 243.7 / 242.0 GB/s per APU (flat from 38 up). At
32 MiB per pair: 212 GB/s per APU (the kernel's ramp; 38–76 blocks best, 304 blocks −7 %).

**Verdict H4: yes.** The one-kernel push drives all three links concurrently at 93–97 % of the single-link rate
(244–254 GB/s per APU against 3 × 87–89), sending and receiving at once; the three links do not serialise. No peer
ordering or stream arrangement beats it by more than 2.4 % (contiguous thirds: a candidate for a switch, not worth a
change on its own), and the copy engines are 45 % slower. `amd-smi xgmi`: each APU pair is two xGMI ports (6 of 8 ports
up per APU), 32 Gb/s × 16 lanes each = 128 GB/s raw per direction per pair; the push reaches 70 % of raw. **The E9
model's 273 GB/s per APU (assumed) is 244 measured** (−11 %).

**H4 in the 4e10 pipeline (measured, one node, job 21035):** with `COMM_XGMI_STATS=1` the 4 × 10^10 run pushes
962 GB per APU (320.7 GB per link) in 3252 pushes, 4.2–4.3 s of push kernel = **221–230 GB/s per APU, 74–80 GB/s on
each of the three links at once** (84–91 % of the one-link rate): the three links are saturated together in the real
pipeline too. The stamps cost nothing measurable: 81.64 s with them, 81.57 s without (one run each, digits identical).
(`amd-smi xgmi`'s accumulated per-pair counters moved by ~0.9–1.4 TB per pair per direction over the run — they count
more than the push's payload, other traffic included; not used.)

In the (small-exchange) multi-process runs the links run below their bandwidth, because the pushes are small: lbench 2^26 over two node-processes
(7.9 MB pushes) 55–58 GB/s per link; ecalc 10^8 at size 2 (0.6 MB average) 15 GB/s per link (`~/x13/b1b/`). At the
target's top levels the pushes are hundreds of MB and bandwidth-bound.

#### 3. E9 part 1 — the present overlap

##### 3.1 The stage is not the link (measured, loopback, `~/x13/b1b/`)

The xGMI *stage* of the layered exchange as the host sees it — intra post to intra wait return — is **40–100×** its
link time: 2^24–2^26 lbench, stage 0.037–0.17 s per APU against 0.0006–0.0017 s of push kernel (1–2.4 %). The stage
contains, besides the push: the wait for the producer (the push is queued on the transfer stream behind the pack kernel
of the same chunk, so the host's intra wait also waits for the row pass + twiddle-pack), the four APU threads' barrier
(skew), the launch and the stream sync. So "xGMI-busy" must be counted at the link (the stamped push kernels); the
stage-level numbers are reported too, but the both-busy fraction below is at link level.

##### 3.2 Two node-processes on one node (loopback TCP, measured; the substitute until two real nodes are free)

| run | path | exchanges | xGMI link time hidden under the fabric (both / link) | stage both/xGMI | fabric busy / span (node view) | held | fabric idle while held |
|---|---|---|---|---|---|---|---|
| lbench 2^24, 2 reps | pow2 (pipelined) | 96 / APU | **74.9 – 75.5 %** | 60–66 % | 98.9 % | 0.077–0.083 s | 0.0007 s (0.4 % of fabric) |
| lbench 2^26, 2 reps | pow2 | 96 | **73.9 – 76.0 %** | 63–66 % | 99.3 % | 0.24–0.27 s | 0.0008 s (0.15 %) |
| ecalc 10^8, size 2 (VERIFY OK, identical) | pow2 | 3508 | **75.1 %** (node view 70.5 %) | 60 % | 93.5 % | 0.94 s | 0.022 s (1.1 %) |
| ecalc 10^8, size 2, `DIST_GEN=1` (identical) | **general map** (alltoallv) | 3508 (3456 v) | **1.1 %** (node view 1.2 %) | 3 % | 91.2 % | 0.009 s | 0.001 s |

**Structural ceiling of the present pipeline (pow2 path): 3/4.** `ntt_dist` cuts a transform into K = 4 chunk
exchanges; with two in flight the intra stage of chunk k runs while chunk k−1 is on the wire — except chunk 0, whose
predecessor is the previous transform's last chunk, which `dist_fwd_post` / `dist_inv_post` have already waited for.
So 3 of 4 pushes can hide, and 74–76 % is measured: the pow2 path collects everything its structure allows.

**The general map collects nothing.** `y_alltoallv` completes the previous v-exchange (its inter wait) *before* its own
intra stage (`complete_v` at the top: "one kind at a time" and one v-exchange pending), so xGMI and the fabric never
overlap there. The general map is what every non-power-of-two group runs — 3, 6, 192 and **576** nodes, i.e. at the
target the two top tree levels, the reciprocal's and the division's full-machine products.

**One-at-a-time does not bind (on a fabric-bound pipeline).** The held time is large (an exchange ready for the fabric
waits ~45 % of the fabric time for its predecessor), but in all of it except 0.15–1.1 % the fabric was still busy with
that predecessor: the NIC is continuously busy (node view: fabric busy 93–99 % of the exchange span). A second inter
exchange in flight would share the same bandwidth; it would only recover the ≤ 1 % gaps.

##### 3.2b Three node-processes on one node (loopback TCP, measured, job 21035, `~/x13/b4/e8_3loop_m*.txt`)

ecalc 10^8 at size 3, POOL_LOG 27, `COMM_LAYER_STATS=1` / `2` with `COMM_XGMI_STATS=1`; VERIFY OK on all 3, digits identical.
The schedule at size 3: level 1 pairs nodes 0–1 (pow2 path, g = 2), the top level and the division run over all
three (general map, g = 3); node 2 takes part only in the g = 3 exchanges.

| node | xGMI link time hidden under the fabric (passive / exact) | fabric busy / span | held (of it fabric idle) | per-APU fabric bytes max/min |
|---|---|---|---|---|
| 0 (g = 2 and g = 3) | 22.6 % / 18.3 % (node view 24.7 / 25.5 %) | 91.4 % | 0.106 s (0.011) | 1.0000 |
| 1 (g = 2 and g = 3) | 22.2 % / 18.7 % | 90.4 % | 0.098 s (0.011) | 1.0000 |
| **2 (g = 3 only)** | **0.0 % / 0.0 %** | 89.5 % | 0 | 1.0000 |

So at three node-processes the whole-run fraction is ~20 % (the pairwise level's 75 % diluted by the g = 3 products'
0 %), and a node that sees only the general map hides nothing. **NIC balance: the four APU threads send identical
bytes (max/min 1.0000) — measured**; fabric time differs by 1–3 %.

##### 3.3 Two and three real nodes — MODELLED (not measured)

No 2- or 3-node allocation was possible this session: the admin CI array (cdash-nightly, 12 tasks) held the pool for
hours, then s24-30 went down (rebooted) and s24-16 went to another user. Job 21019 (-N2, `~/x13/b5.sh`) was queued
and never started; it was cancelled. The 3-node slot never happened. What follows is modelled from the loopback
measurements and the code's structure, and is labelled so.

* **2 real nodes (pow2 path, TCP over the 1 GbE `enp129s0`, 0.117 GB/s per node — measured link speed 1000 Mb/s).**
  Link-level both-busy is set by the structure (3 of K = 4 pushes run under the previous chunk's inter stage) as long
  as the inter stage of chunk k−1 outlasts the push of chunk k. Over 1 GbE the inter stage is ~30× longer than over
  loopback (3.3 GB/s), so the condition holds with an even larger margin: **modelled both/xGMI-link ≈ 75 %**,
  fabric busy/span ≥ 95 %, fabric idle while held ≤ 1 %. The fabric is the bottleneck more strongly than in the
  loopback runs, so the one-at-a-time verdict (does not bind) is if anything firmer.
* **3 real nodes.** The top level (g = 3), the reciprocal and the division run the general map (v-exchanges): **modelled
  both ≈ 0 %** there (measured 1.1 % for the same code path at size 2 with `DIST_GEN=1`). The level-1 group of two
  nodes runs the pow2 path (≈ 75 %). By bytes the g = 3 products dominate the fabric traffic (the top-level, reciprocal
  and division products are the largest), so the run as a whole hides **well under a quarter** of its xGMI link time —
  modelled, not measured.
* **Imbalance (part 3).** The general map's parts are `partn(R, nr, rho)` = ⌊R/nr⌋ or ⌈R/nr⌉ rows, so per-APU fabric bytes
  differ by at most one row in R/nr ≥ 32 rows per chunk (≤ 3 %, and ≈ 0 at the target's R/nr of thousands) — modelled
  from the code; the NIC-balance line of the stats (per-APU fabric bytes max/min) will measure it on the first
  multi-node run.

#### 4. The overlap ceiling against the §29 E9 estimate (modelled)

E9 assumed T_intra/T_inter = 0.37 at 576 (xGMI 273 GB/s per APU carrying S, the NIC 100 GB/s carrying S(g−1)/g) →
ceiling 27 % of the sequential exchange time. With what was measured:

* the push is 244 GB/s per APU, not 273 (H4), and in the layered form an APU sends 3/4 of its slabs over xGMI (the
  quarter bound for its own APU index stays local): T_intra/T_inter = 0.75 × 100/244 × g/(g−1) = **0.31** at 576
  (0.41 at g = 4) → **ceiling 23.5 % at 576** (29 % at g = 4), modelled.
* the pow2 path already collects 3/4 of it (measured): 0.31 × 1/4 exposed → 1.077 instead of 1.31 sequential — the
  present saving is **17.8 %** of the sequential exchange time, **5.9 %** remains (the chunk-0 push).
* the general map (192- and 576-node levels) collects **0** (measured 1 %): all **23.5 %** remains there.

#### 5. Verdict — are E9 parts 2 and 3 worth writing?

(from the loopback measurements and the structure; the real-node confirmation is the first item for the next session:
`sbatch ~/x13/b5.sh` for 2 nodes, `~/x13/b3.sh <job>` for 3)

* **Part 2, yes — but a different part 2 from the one §29 describes.** The limit that binds is not the
  one-at-a-time inter stage (it does not bind while the fabric is the bottleneck: fabric idle while held ≤ 1 %) and
  not the whole-exchange granularity of the pow2 path (it already hides 75 %, its structural maximum with K = 4). It is
  the **general map's v-exchange, which never overlaps at all**, and it is exactly the path of the target's largest
  products. Writing the v-exchange with the equal one's two-slot pipeline (post intra k while inter k−1 is on the wire;
  `gen_fwd` / `gen_inv_pw` already post K chunks back to back) is worth up to 23.5 % of the exchange time on those
  levels (modelled), for a small change in `comm_layered.c` (a second v slot).
* The pow2 path's remaining 1/K (5.9 % of exchange time at K = 4) can be cut with the existing switch `DIST_CHUNKS=8`
  (1/8) before any code — measure it.
* Allowing more than one inter exchange in flight: not worth writing on present evidence (≤ 1 %).
* **Part 3 (xGMI as a relief valve for NIC imbalance): not worth writing.** The per-APU fabric bytes differ by the
  row rounding of the general map only (⌊R/nr⌋ or ⌈R/nr⌉ rows: ≤ one row in R/nr, modelled ≤ 3 % on aac6's small
  shapes and ≈ 0 at the target), so the relief valve's ceiling — the imbalance — is nil. Not measured on real nodes.

#### Gate

`./mnaccept.sh 21035 --only unit,e9,mn` on `p13-X` @ 59ebca79 (aac6-spx-b): **15 passed, 0 failed**, 1310 s
(unit ×8, e9 size 1 both bases identical, mn e8 sizes 2/3/4 and e9 sizes 2/4 identical). The later commits touch
`results/EXCHANGE.md §X13` only.

#### Open issues

* Real 2- and 3-node measurements not made (pool unavailable) — the 2/3-node numbers above are loopback-measured or
  modelled. Scripts ready: `sbatch ~/x13/b5.sh` (2 nodes, self-running), `~/x13/b3.sh <job>` (3 nodes).
* `COMM_LAYER_STATS=2` (exact) perturbs slightly (the watcher completes the inter stage early); use 1 for timings.
* The xGMI *stage* in the host's view contains the producer's pack kernel and the four-thread barrier; only the
  stamped link time (`COMM_XGMI_STATS=1`) measures the xGMI itself.
* Block transposes in the layered exchange are g × 4 `hipMemcpyAsync` calls per direction (0.1–0.27 ms per exchange
  at g = 2–3, measured); at g = 576 that is 2304 launches per transpose — a one-kernel transpose will be needed there
  (not measured at scale; modelled from the per-copy cost).

#### RESUME

* Done: instrumentation + drivers; H4 (one node, and in the 4e10 pipeline); E9 part 1 at 2 and 3 node-processes on
  one node (loopback); the gate. No job live.
* Next session: the real 2-node batch (`sbatch ~/x13/b5.sh`) and the 3-node slot (`~/x13/b3.sh`); then, if the verdict
  holds, E9 part 2 as the pipelined v-exchange.

## X13b — the uneven exchange two deep, one-kernel transposes, faster packs (PLAN.md §31 row X, axis A)

(former file: X13b.md)

Branch `p13b-X` (from `p13b-step0` @ c988218). Files: `ecalc/comm_layered.c`, `ecalc/ntt_dist.c` (the pack/transpose
kernels, a bench function appended), `ecalc/tests/t_dist.c` (drivers `DIST_PBENCH`, `DIST_TBENCH`, and a pipelined
alltoallv check). Nothing else touched. Every addition is behind a switch, off by default; the defaults are step 0's.
aac6 clone `~/ntt-X13b`; batches `~/x13b/b*.sh`, logs `~/x13b/b*/`.

Labels: **measured** = on aac6 in this session; **modelled** = arithmetic on measured constants; **assumed** = PLAN §25.

#### M-RUN SWITCHES

Axis A (uneven-exchange depth). Environment for each value:

| value | environment |
|---|---|
| A = 1 (today, default) | `COMM_ALLTOALLV_DEPTH=1` (or unset) |
| A = 2 | `COMM_ALLTOALLV_DEPTH=2` |

**Important for the M-run's "10¹⁰ over 4 processes" rows:** at 4 node-processes every transform group is a power of
two (g = 2, 4), so the products run the equal-slab pipeline and **the v-exchange depth never acts** — depth 1 and 2
would measure the same code. To measure axis A there, add `DIST_GEN=1` to *both* rows (the products then run the
general map, as the target's 3-, 192- and 576-node levels do), i.e.

    A=1:  DIST_GEN=1 COMM_ALLTOALLV_DEPTH=1
    A=2:  DIST_GEN=1 COMM_ALLTOALLV_DEPTH=2

or run 3 node-processes (g = 3 at the top level, the reciprocal and the division), where the general map is used
without forcing. Both are byte-identical (below). At size 1 (the 4 × 10¹⁰ rows) axis A does nothing (no layered
communicator).

Other options of this branch (not table axes; each independent, off by default, bit-identical):

| option | environment | acts where |
|---|---|---|
| one-kernel transposes | `COMM_LAYER_TKERNEL=1` | the layered exchange (sizes ≥ 2), both kinds |
| twiddle-pack by row recurrence | `DIST_TWREC=1` | `ntt_dist`'s forward pack and inverse unpack (every size, incl. size 1's four-step over xGMI) |
| tiled transposed-inverse pack | `DIST_TPACK=1` | `dist_inv_t` only (tests; no production path) |

Instrumentation (off for timings): `COMM_LAYER_STATS=1` (passive) / `2` (exact) and `COMM_XGMI_STATS=1` print the
stage timeline and the both-busy fraction at exit (results/EXCHANGE.md §X13 §1); the report now also prints the depth, the
transpose form and the scratch of one APU thread's communicator.

#### 1. What was built

##### `COMM_ALLTOALLV_DEPTH=2` — the v-exchange two deep (`comm_layered.c`, `y_alltoallv2`)

Depth 1 (unchanged) completes the previous v-exchange (its inter wait and scatter) at the top of the next post, so the
xGMI stage of exchange k never runs under the fabric stage of k − 1 (X13: 1.1 % both-busy). Depth 2 reorders the post:
count tables, intra stage and reorder of exchange k first (under k − 1's inter stage), then complete k − 1, then post
k's inter stage. The inter transport still takes one exchange at a time, and "posting k completes k − 1" still holds
on return — `gen_inv_pw` unpacks chunk k − 1 right after posting k without a wait, which stays correct.

Buffers: the inter stage's send x2 (A bytes) and receive x3 (B bytes) are busy while an exchange is pending, so they
alternate between two slots of the communicator's own memory. The intra receive x1 (A) lives in the slot's x3 area
(dead once reordered into x2, before the inter stage receives into x3); the x0 area (only when the caller's slabs are
not back to back) is shared. Scratch per APU thread: depth 1 `x0 + 2A + B`, depth 2 `x0 + 2(A + max(A, B))` — with the
general map's A ≈ B = S: **3 S → 4 S, +S (one more exchange's slab buffer)**. Measured (the stats' scratch line,
largest communicator of one APU thread, ecalc 10⁸): size 2 `DIST_GEN=1` **12.6 MB → 16.8 MB** (+33 %), size 3
**8.5 → 11.2 MB** (batch 2); the first version (x1 kept separately, 5 S) measured 21.0 MB (batch 1). Per node that is
4 × the per-APU figure; at the target the v-exchange's S is one chunk of the plane per APU (plane/K, K = 4), so depth 2
costs one more quarter-plane of scratch per APU on the general-map levels (modelled from the layout).

##### `COMM_LAYER_TKERNEL=1` — the transposes as one kernel

`block_transpose` (the equal path's two transposes: 4 g blocks each) becomes one kernel (`k_btrans`: grid.y = the
destination block, 16-byte words, coalesced both sides) instead of 4 g `hipMemcpyAsync`. The v-exchange's three
reorder copy loops (x0 pack, x1 → x2, the scatter into the caller's receive buffer; 4 g variable-length copies each)
become one batched-copy kernel each (`k_bcopy`, a (dst, src, len) table in pinned host memory). Falls back to the
copies when a pointer is not device-accessible or a length is not a multiple of 8/16 B.

##### Packs (`ntt_dist.c`)

* `k_pack` — the plain pack K13 measured at 360–770 GB/s — **is not on any production path**: the forward packs with
  `k_twpack`, which was already LDS-tiled and fuses the twiddle. `k_pack_tiled` is its tiled copy (bench only).
  The only other scattered-store pack is `k_pack_t` (the transposed inverse `dist_inv_t`, tests only):
  `k_pack_t_tiled` behind `DIST_TPACK=1`.
* The production twiddle-pack (`k_twpack`) and its inverse (`k_unpacktw`) run at **half the copy rate** (measured,
  below): the twiddle w_n^(ij) gathers two table entries per point, scattered by the bit reversal of j.
  `DIST_TWREC=1` (`k_twpack_r` / `k_unpacktw_r`): a thread's four rows i, i+8, i+16, i+24 of column j take
  w_n^(ij) from the tables once and step by w_n^(8j) (one per column, in LDS) — two gathers per four points instead
  of eight; `ec_mm` is canonical, so the product chain is exact (bit-identical, checked). `DIST_TWREC=2`: eight rows
  per thread (block 32 × 4, step w_n^(4j)).

#### 2. Measurements

##### 2.1 Both-busy on the general map (measured, batch 1, job 21052, s24-30; loopback TCP, ecalc 10⁸, POOL_LOG 27)

Link level = the stamped push kernels (`COMM_XGMI_STATS=1`) ∩ the fabric stages: the fraction of the xGMI link time
hidden under the fabric (X13 §3.1: the host-side "stage" is 10–20× the link time, so this is the measure).

| run | depth | xGMI link time hidden (node 0) | node view | stage both/xGMI | span per APU | fabric busy/span | ecalc total |
|---|---|---|---|---|---|---|---|
| size 2, `DIST_GEN=1`, passive | 1 | **1.5 %** | 1.5 % | 4.2 % | 1.931 s | 93.5 % | 8.09 s |
| size 2, `DIST_GEN=1`, passive | **2** | **75.1 %** | 74.9 % | 57.0 % | 1.665 s (−14 %) | 96.7 % | 7.44 s |
| size 2, `DIST_GEN=1`, exact | 1 | 1.5 % | 2.1 % | 3.9 % | 2.044 s | 90.9 % | 8.18 s |
| size 2, `DIST_GEN=1`, exact | **2** | **70.1 %** | 76.0 % | 51.8 % | 1.785 s | 95.5 % | 7.69 s |
| size 3, node 2 (g = 3 only) | 1 | **0.0 %** | 0.0 % | 0.0 % | — | 88.6 % | 8.26 s |
| size 3, node 2 (g = 3 only) | **2** | **72.6 %** | 70.3 % | 43.3 % | — | 93.7 % | 8.05 s |
| size 3, node 0 (g = 2 and 3) | 1 | 22.9 % | 23.1 % | 30.1 % | — | 90.3 % | |
| size 3, node 0 (g = 2 and 3) | **2** | **73.1 %** | 72.2 % | 57.0 % | — | 92.9 % | |

**The general map now hides 70–75 % of its xGMI link time, the pow2 path's structural ceiling (3 of K = 4 chunk
pushes; X13 measured 74–76 % there).** Size 3 as a whole goes from ~23 % (pairwise level only) to ~73 %. The
held time (an exchange ready for the fabric waiting for its predecessor) rises from 0.01 to 0.78 s per APU at size 2 —
it is the one-at-a-time inter stage now doing its job — and of it the fabric was idle only 0.017 s (2 %). The ecalc
totals are single runs on loopback (noise ~0.3 s); the span per APU (−14 % at size 2) is the cleaner number.
The ceiling of the saving on the whole exchange time is small on this loopback shape (min/(x+f) = 6–13 %, since the
fabric is 15× the xGMI stage here); at 576 nodes it is the 23.5 % of X13 §4 (modelled), and depth 2 now collects
3/4 of it on the general-map levels as on the pow2 ones (modelled as in X13 §4: 17.8 % of the sequential exchange time on those levels, 5.9 % left).

##### 2.1a Two real nodes (measured, job 21068, s24-16 + s24-26, TCP over the 1 GbE; ecalc 10⁸, POOL_LOG 29, `DIST_GEN=1`)

The first real multi-node measurement of the layered exchange (X13's §3.3 was modelled). One node-process per node;
fabric 0.025 GB/s per APU (0.1 GB/s per node, the 1 GbE); every run identical, both nodes VERIFY OK.

| run | xGMI link time hidden (node 0 / node view) | stage both/xGMI | fabric busy/span | held (of it fabric idle) | phases / total |
|---|---|---|---|---|---|
| general map, depth 1, passive | **1.4 % / 1.7 %** | 27.4 % | 99.1 % | 0.15 s (0.0004) | 25.3 / 30.73 s |
| general map, **depth 2**, passive | **74.3 % / 73.0 %** | 61.8 % | 99.5 % | 7.69 s (0.020) | 19.8 / 24.77 s |
| general map, depth 2, exact | 62.5 % / 74.7 % | 49.2 % | 99.3 % | 7.71 s (0.059) | 20.0 / 24.78 s |
| pow2 path (reference), depth 1 | 74.9 % / 72.7 % | 57.0 % | 97.3 % | 9.30 s (0.020) | 19.7 / 24.93 s |
| general map, depth 1, no stats | — | — | — | — | 20.3 / 25.36 s |
| general map, **depth 2**, no stats | — | — | — | — | 19.6 / 24.54 s |

**On real nodes depth 2 takes the general map from 1.4 % to 74 % of the xGMI link time hidden — the same as the pow2
path (74.9 %) and as X13 modelled for two real nodes (≈ 75 %).** The held time grows to 7.7 s because the one inter
stage in flight now always has a successor ready; the fabric was idle in only 0.3 % of it (one inter exchange at a
time does not bind — X13's verdict holds on real nodes). With the 1 GbE the fabric is 99 % of the span, so the
saving on the wall is small here: the ceiling min(x, f)/(x + f) is 1.2–1.4 %; uninstrumented phases 20.3 → 19.6 s
(−3.4 %, one run each — the instrumented depth-1 run at 25.3 s shows single runs spread more than that). At the
target's 100 GB/s per APU (assumed) the ratio T_intra/T_inter is 0.31 (X13 §4, modelled), and the 3/4 hidden is worth
17.8 % of the sequential exchange time on the general-map levels (modelled).

##### 2.1b Depth 1 vs 2 without instrumentation (measured, batch 6, job 21063, s24-16; loopback, 10⁹, POOL_LOG 29)

Two runs each, alternating, all identical to the reference with every node VERIFY OK. Phases = bs + 10dP + dm + dc.

| run | depth 1: phases / total | depth 2: phases / total | phases |
|---|---|---|---|
| size 3 (general map at g = 3) | 15.6, 15.6 / 20.61, 20.27 s | 14.5, 14.4 / 19.48, 19.39 s | **−7.4 %** |
| size 4, `DIST_GEN=1` (general map at g = 2, 4) | 12.6, 12.7 / 18.03, 17.60 s | 11.5, 11.3 / 16.69, 16.48 s | **−9.9 %** |

On loopback TCP (3.3 GB/s) the exchanges are a large part of these runs; with a slower real fabric the fabric stage
dominates more and the xGMI part that depth 2 hides is a smaller share (the both-busy fraction itself holds, X13 §3.3).

##### 2.2 Transposes (measured)

In the exchange (block transposes per exchange, from the stats; loopback 10⁸):

| run | copies | one kernel |
|---|---|---|
| size 2 (pow2 path) | 0.076–0.082 ms | 0.040–0.042 ms |
| size 3 (both paths) | 0.092–0.123 ms | 0.047 ms |

Alone, one APU (`DIST_TBENCH=1`, median of 7, host wall incl. launches and the stream sync; batch 1):

| g (4 g blocks) | 4 KiB blocks: copies / kernel | 64 KiB | 1 MiB |
|---|---|---|---|
| 2 | 0.022 / 0.010 ms | 0.022 / 0.009 | 0.025 / 0.012 |
| 4 | 0.066 / 0.009 | 0.070 / 0.010 | 0.071 / 0.016 |
| 16 | 0.240 / 0.010 | 0.249 / 0.011 | 0.259 / 0.045 |
| 64 | 0.979 / 0.010 | 0.992 / 0.015 | 0.925 / 0.206 |
| 192 | 2.902 / 0.010 | 2.807 / 0.035 | 2.676 / 0.545 |
| **576** | **8.160 / 0.012 (662×)** | 7.918 / 0.107 (74×) | 7.013 / 1.522 (4.6×) |

The copies cost 3.0–4.4 µs each regardless of size (launch-bound until ~1 MiB); the kernel runs at 2.8–3.2 TB/s on
large blocks. At g = 576 a transpose of small blocks drops from 8 ms to 0.01 ms — and an exchange has two of them per
APU. All outputs identical.

##### 2.3 Packs (measured, `DIST_PBENCH`, one APU, a plane of 2^L points split over 4 ranks, median of 5, rate = 16 B × points / t)

| kernel | 2^24 | 2^26 | 2^28 | 2^30 | 2^32 |
|---|---|---|---|---|---|
| device copy (reference) | 3277 | 3251 | 2896 | 3039 | 2911 GB/s |
| **k_twpack** (fwd pack, tiled + twiddle; production) | 1533 | 1324 | 1201 | 1245 | 1253 |
| k_pack (plain; not used) | 566 | 418 | 359 | 372 | 300 |
| k_pack_tiled (13b) | 3018 | 3056 | 2640 | 2995 | 2779 |
| k_unpack (fwd unpack; production) | 2863 | 2293 | 2170 | 2507 | 2267 |
| k_pack_cols (inv pack; production) | 2778 | 2331 | 2636 | 2614 | 2280 |
| **k_unpacktw** (inv unpack, tiled + twiddle; production) | 1501 | 1492 | 1195 | 1297 | 1278 |
| k_pack_t (dist_inv_t, plain) | 591 | 429 | 356 | 372 | 301 |
| k_pack_t_tiled (13b, `DIST_TPACK=1`) | 2980 | 3139 | 2614 | 3035 | 2755 |
| k_unpack_t (dist_inv_t) | 2868 | 2593 | 2432 | 2495 | 2366 |

K13's 360–770 GB/s pack is `k_pack`, which production does not use; tiling it (or `k_pack_t`) reaches the copy rate
(×5–9, bit-identical). The production forward pack and inverse unpack are the twiddled ones, at 1.2–1.5 TB/s: at
2^30 split over 4 (2^28 points per rank) `k_twpack` takes 3.45 ms (the per-rank rows' forward is ~11.5 ms, K13) —
that is where `DIST_TWREC` aims:

| kernel (batch 2 / 3, bit-identical to the production form) | 2^26 | 2^28 | 2^30 |
|---|---|---|---|
| device copy | 3236–3429 | 3061–3100 | 3399–3424 GB/s |
| k_twpack (production) | 1360–1404 | 1223–1310 | 1347–1350 |
| **k_twpack_r, `DIST_TWREC=1`** | 1864–1967 | **2322–2335** | **2223–2236** (×1.65) |
| k_twpack_r<4>, `DIST_TWREC=2` | 1990 | 2009 | 2083 |
| k_unpacktw (production) | 1486–1570 | 1203–1315 | 1461–1463 |
| **k_unpacktw_r, `DIST_TWREC=1`** | 1869–1893 | **1995–2006** | **2087–2097** (×1.43) |
| k_unpacktw_r<4>, `DIST_TWREC=2` | 1959 | 1939 | 1860 |

`DIST_TWREC=1` is the better form at the large sizes. **End to end it does not show at size 1**: 4 × 10¹⁰, one
node, one run each (batch 3, job 21058, s24-16, digits identical): `DIST_TWREC=0` 46.5 s of phases (bs 25.4, dm
21.1; total 66.8 s), `DIST_TWREC=2` 46.7 s (bs 25.5, dm 21.1) — the four-step's multi-device products (`mdev` 8.4 s)
spend only a small part in these kernels. It acts on every `ntt_dist` transform, so its weight grows with the share
of the distributed products (multi-node); not measured there. Not proposed for the table.

#### 3. Correctness (measured)

Batch 1 (job 21052): `COMM_ALLTOALLV_DEPTH=2 COMM_LAYER_TKERNEL=1 DIST_LAYERED=1 mnrun 2 t_dist 24` VERIFY OK (12
checks, incl. the layered alltoallv and the new pipelined one: three v-exchanges posted back to back, then three
waits); `DIST_TBENCH` 21/21 identical; `DIST_PBENCH` the tiled packs bit-identical at every size. ecalc, every run
`mn: all nodes VERIFY OK` and the concatenated digits `cmp`-identical to ref/e_<d>.txt:

| digits | size | settings |
|---|---|---|
| 10⁸ | 2 | `DIST_GEN=1`, depth 1 and 2, stats passive and exact (4 runs) |
| 10⁸ | 3 | depth 1, depth 2, depth 2 + one-kernel transposes |
| 10⁸ | 2 | transposes copies / one kernel |
| 10⁸ | 4 | depth 2 + one-kernel transposes |
| 10⁹ | 3 | depth 2 + one-kernel transposes (19.3 s) |
| 10⁹ | 2 | `DIST_GEN=1`, depth 2 + one-kernel transposes (22.7 s) |

Batch 2 (job 21055, s24-16): `DIST_TWREC=1 t_dist 26` VERIFY OK (68 checks: the distributed convolutions on the
synthetic communicator); `COMM_ALLTOALLV_DEPTH=2 COMM_LAYER_TKERNEL=1 DIST_TWREC=1 DIST_LAYERED=1 mnrun 2 t_dist 24`
VERIFY OK (12, both processes); ecalc 10⁸ identical: size 2 `DIST_GEN=1` depth 2 (after the aliasing), size 3 depth 2,
size 3 with all three switches; 10⁹ size 1 `DIST_TWREC=0/1` two runs each identical.
**`./mnaccept.sh 21055 --only e9,mn` at the defaults: 7 passed, 0 failed, 130 s** (e9 both bases, mn e8 sizes 2/3/4,
e9 sizes 2/4). Batch 3 (job 21058): `DIST_TWREC=2 t_dist 24` VERIFY OK; 4 × 10¹⁰ at `DIST_TWREC=0` and `2` identical
to results/e_4e10.out.

Batch 4 (job 21060, s24-16), **the gate's unit steps at the defaults**: t_mn_grid (2 procs) 200 checks, t_mul 20
189, t_bs 10, t_dbig 0 555, t_newton 20 620, t_verify 334, t_out 1 — all VERIFY OK. Batch 7 (job 21067): t_ntt 24
VERIFY OK (1519 checks, 613 s). **So the gate `unit,e9,mn` is green at the defaults, split over jobs 21060, 21067
(unit, p13b-X @ 9320047) and 21055 (e9, mn, @ 9aa6481 — 9320047 only adds `DIST_TWREC=2`, off by default)**; later commits touch results only.
**`COMM_ALLTOALLV_DEPTH=2 COMM_LAYER_TKERNEL=1 DIST_TWREC=1 ./mnaccept.sh 21060 --only e9,mn`: 7 passed, 0 failed,
138 s** — e9 size 1 both bases, mn e8 sizes 2/3/4 and e9 sizes 2/4 identical with every switch on.

#### RESUME

* Done: depth 2, the transpose kernels, the tiled packs, the twiddle recurrence, the benches; batches 1–3 (above).
* Batches 4, 6, 7 done (above): the gate is green.
* The 2-node batch ran (job 21068, §2.1a). Nothing pending; no job held. All work is done.

## A07 (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products

(former file: A07.md)

CPU only, existing logs, no cluster job, no engine change. 2026-10-11, analysis by agent a07.

#### Answer
- **The gradient is still there in today's code**, at every size (2, 4, 6 nodes), and it is reproducible to +-0.15 s (9 runs, 4 nodes).
- **Cause (evidence: measured correlation + code read, not a controlled experiment):** the ranks do equal work but receive unequal amounts
  of result. Each rank receives the part of the product that falls in *its own equal share of the accumulator C*
  (`mn_win_chunk` / `piece_window`, `mdb_share` gives equal shares). A product is a grid of pieces (`mn_grid`, ka x kb pieces at shift oa+ob);
  the pieces overlap most in the middle of C, so the middle ranks receive up to 1.3-1.55 x the mean and the end ranks about 0.6 x.
  Across 8 rank-points per size the result-exchange wait is **inversely proportional to the received volume** (Pearson r = -0.96 .. -1.00 at 4 and 6 nodes):
  the ranks that receive least (rank 0 in dm; ranks 0 and last in bs) wait longest, because they finish their (smaller) scatter/add work
  earlier and then wait for the heavy receivers at the next exchange.
- **Not the cause:** work split per rank (equal rows per rank by construction), exchange order (S27: the rotated peer order gave the same shape, measured at 10 nodes),
  rank-0 extra duties (bs shows the same wait at the last rank as at rank 0, and the wait tracks received GB, not rank number).
- **Recommendation: NO-GO** for building a rebalancing fix now. Modelled payoff at 10 nodes about -7 s (range -5..-10), about 2 % of wall, below the
  single-run noise (sd 8-10 s); the fix touches the share layout used by everything. Details and the sketch below.

#### Per-rank table (seconds per APU thread = thread-sum / 4, the S27 convention; `wait` = blocked in wait; `roff` = waiting for a peer to arrive; GB = received per node, measured)
Source: `~/s48/log/n{2,4,6}_r{1,2}.log` (mean of the two runs; they agree within 0.1 s), phase result exchange (kind `result`).

| nodes | phase | rank | GB recv | wait s | roff s | sig s |
|---|---|---|---|---|---|---|
| 2 | dm | 0 / 1 | 81 / 153 | 2.9 / 2.2 | 1.0 / 0.3 | 0.2 / 1.0 |
| 2 | bs | 0 / 1 | 57 / 57 | 1.3 / 1.3 | 0.4 / 0.4 | 0.3 / 0.3 |
| 4 | dm | 0,1,2,3 | 43, 200, 272, 186 | 7.1, 5.7, 4.9, 5.6 | 3.2, 1.7, 0.8, 1.4 | 0.3, 1.0, 1.3, 1.1 |
| 4 | bs | 0,1,2,3 | 112, 196, 187, 103 | 4.5, 3.8, 3.8, 4.5 | 1.5, 0.9, 0.8, 1.6 | 0.6, 1.0, 1.0, 0.6 |
| 6 | dm | 0..5 | 66, 228, 426, 486, 402, 319 | 17.8, 16.4, 14.4, 14.0, 14.7, 16.0 | 8.0, 6.5, 4.7, 3.6, 4.7, 5.3 | 0.5, 1.2, 2.3, 3.0, 2.1, 2.2 |
| 6 | bs | 0..5 | 149, 258, 312, 312, 250, 142 | 9.2, 8.3, 7.9, 7.9, 8.8, 9.4 | 3.3, 2.5, 2.0, 1.7, 2.7, 3.2 | 0.9, 1.4, 1.6, 1.8, 1.3, 0.8 |

Summary (measured):

| nodes | phase | max/mean recv | rank-0 wait / min wait | mean wait - min wait s | mean wait s | corr(wait, GB) |
|---|---|---|---|---|---|---|
| 2 | dm | 1.31 | 1.32 | 0.31 | 2.6 | -1.00 (2 points) |
| 4 | dm | 1.55 | 1.45 | 0.95 | 5.8 | -1.00 |
| 4 | bs | 1.31 | 1.18 | 0.35 | 4.2 | -0.99 |
| 6 | dm | 1.51 | 1.27 | 1.55 | 15.6 | -0.99 |
| 6 | bs | 1.31 | 1.17 | 0.72 | 8.6 | -0.96 |
| 10 (S27, old code) | dm | n/a | 2.4 (33 vs 14) | 8.3 | 22.3 | n/a |
| 10 (S27, old code) | bs | n/a | about 1.9 | 6 | 12.0 | n/a |

Today's code, 4 nodes, 9 runs `~/sess1011b/t09/log/p*.log` (mean +- sd of run means): dm wait 7.19+-0.15, 5.74+-0.12, 4.94+-0.13, 5.73+-0.14 s
and bs wait 4.53+-0.06, 3.89+-0.07, 3.87+-0.06, 4.58+-0.06 s for ranks 0..3, GB identical to the s48 runs (43/200/272/186 and 112/196/187/103).
`~/t12/real1` (one 4-node run) the same (dm rank 0 28.0/4 = 7.0 s, rank 2 5.0 s). Wall totals: 2 n 144 s, 4 n 182 s, 6 n 283-304 s (measured).
The gradient is smaller at 6 nodes (ratio 1.27) than the "twice" seen at 10 nodes in S27; the ratio grows with n (more pieces in the grid, steeper overlap).

#### Why the volumes differ (code read, `ecalc/rns_dist.c`)
- `mn_grid` splits a large product into ka x kb pieces and calls `mn_core` per piece at shift oa+ob; the piece's CRT output rows go to the nodes whose C share
  meets [shift, shift+Np) (`piece_window`), exchanged by `comm_alltoallv` tagged `XK_RESULT` (line 1784).
- The coverage multiplicity of the pieces over C is triangular (1,3,5,7,7,5,3,1 units for k = 4: peak/mean 1.75; k = 2: 1.5; k = 1 direct: 1.0), so the middle shares receive most.
  The dm products are truncated (`w < N`, `lowcut`), which moves the peak: at 4 nodes the dm profile is 43/200/272/186 (low end starved, no high end cut), bs symmetric 112/196/187/103.
- Per-node receive work (scatter `k_scatter_mn`, spill add, `mn_round_add`) is proportional to the received window, so the light ranks reach the next exchange earlier and wait (roff).
- Evidence level: the correlation is measured (3 sizes, 18 rank points, same GB in every run); the profile shape agrees with the piece-overlap model; the causal chain
  (volume -> post-processing time -> arrival skew) is inferred, not isolated (would need a run with a per-round arrival timestamp, or with C shares skewed).

#### Modelled payoff
Removing the volume imbalance cannot make the light ranks faster; it can only shorten the critical (heaviest) rank. Model: saving = (max recv - mean recv) x cost per GB at the heaviest rank,
cost per GB = (wait - roff) / GB there (transfer + flush, no skew).
- 4 n: dm 97 GB x 0.0147 s/GB = 1.4 s, bs 46 GB x 0.016 = 0.7 s, total 2.1 s of 182 s (1.2 %), 21 % of the result-exchange wait (10.0 s).
- 6 n: dm 164 GB x 0.0215 = 3.5 s, bs 75 x 0.0196 = 1.5 s, total 5.0 s of ~293 s (1.7 %), 21 % of the result-exchange wait (24.1 s).
- **10 n (modelled):** 21 % x (22.3 + 12.0 = 34.3 s, S27) = **-7 s**, range -5..-10 s (about 2 % of the ~360 s wall). Consistent with S27's -5..-12 (assumed) estimate, not larger. Upper bound: assumes perfect balance and no new cost.
- **576 n (assumed, not measured):** the result-exchange wait share of wall grows with n (5.5 % at 4 n, 8 % at 6 n, 9.5 % at 10 n); if it reaches 10-15 % at 576 n and the imbalance ratio stays 1.3-1.5,
  the payoff is 21 % x 10-15 % = **2-3 % of wall** (assumed). The ratio could be larger (more pieces, up to the 2.0 limit of the triangle) or damped by the lockstep rounds; one run at 20+ nodes with XSTATS would settle it.
- Caveat from S27: lowering transfer time elsewhere (rot order, -17.8 s exposed wait) gave 0 on wall, because the saved time was absorbed as straggler waiting. A balance fix would show on wall only if the heavy receivers are the true stragglers.

#### Recommendation: NO-GO now
- Gain about 2 % (modelled) is below what an ABBA can resolve at 10 nodes in less than about 8 rounds, and the straggler-absorption caveat applies.
- Every fix touches the layout of C (all code that assumes equal `mdb_share`: carries, node scans, digit conversion), the risk is out of proportion.
- Revisit if a 20+ node XSTATS run shows max/mean recv above 1.6 or the result exchange above 15 % of wall.

#### Sketch of the fix if it is wanted later
1. Weighted shares: replace the equal split in `mdb_share` by boundaries `N * W(r) / W(g)` with W the cumulative expected coverage (for the grid shape, known from ka, kb, w, lowcut), per product kind
   (dm truncated, bs symmetric). Needs a per-product share map and a re-layout between products with different maps (extra exchange: probably cancels the gain). Not recommended.
2. Better, no layout change: shrink the volume instead. Where pa = pb, the pieces on one anti-diagonal (i+j = const) have the same shift and window; their transform-domain products could be summed
   before the inverse transform and CRT, so one result exchange per anti-diagonal instead of up to k. Needs a CRT bound check (coefficients grow by up to k; `P24` limit 10^60) and is a larger project than C6; unevaluated (idea only).
3. Cheap probe before any build: add a per-round arrival timestamp to `COMM_XSTATS` for `XK_RESULT` (no digit effect, behind the switch) and one 10-node run, to confirm the causal chain.

#### Files
- This file. Analysis scripts were scratch (parse of the `xstats pe ... kind result` lines, per-rank, divided by 4 threads).
- Data copied bounded to local scratch only; nothing added to the repo besides this note.

#### RESUME
Done: analysis and recommendation. Open: optional 20+ node XSTATS run to measure the imbalance ratio at larger n; optional arrival-timestamp probe.

## K1b: raw-once gather (S39 candidate A, O7b lever 3) -- IMPLEMENTED behind DIST_GATHER_RAW (off), probe + gates + ABBA ARMED on aac6

(former file: K1b.md)

Branch k1b (from origin/main a448d73). Labels: **m** measured, **mod** modelled, **as** assumed. Times Eastern.

#### 1. Question
O7b (archive:results/O7b.md, on branch o7b): k_gather is the largest kernel at 6.441e10 on one node (6.34 s APU0, 174 calls x 36.5 ms per APU, 15 % of GPU time, m), plus k_bload 1.2 s. S39 candidate A: gather each operand once without the canon, then canonicalise per prime with a local elementwise pass; O7b modelled 2.5-3.6 s.

Prior evidence that bears on it: S40 (aac7) ran the "local-only source" micro-test that S39 asked for: remote 36.1 ms vs local 35.9 ms with the select kernel (m). So the gather is **not** bound by the remote pull, and S40 concluded "do not build A as specified". That conclusion only removes S39's reason (fewer remote pulls); it does not decide the design. Raw-once saves whenever the gather itself (whatever bounds it: the strided store, the 64-bit `/` and `%` index arithmetic, the remote read) costs much more than a coalesced canon pass, because the gather then runs once per operand instead of once per prime. S50's tile kernel (coalesced store) bought only 5 ms of 35 ms (m), so the store alone is not the bound either. The right micro-test is therefore the design's own two pieces: raw gather vs gather + canon, and the canon pass, at the product shape.

#### 2. Micro-test (tests/t_gather_raw_probe.c, `make tests/t_gather_raw_probe`)
Copies of k_gather<1> on APU 0 with the source striped over the 4 APUs (the product's case), rows 8192 x C 65536 (2^29 points, the 6.441e10 shape, as S40) and rows 1024 x C 524288:
V0 gather + canon (the product), V1 gather raw, V2 canon pass plane -> plane, V3 canon pass in place, V4 gather + canon with a power-of-two index (shift / mask; information only, a possible cheaper lever), V5 raw gather from a local-only source (S40's check repeated on aac6). V2, V3, V4 must equal V0 bit for bit (checked), V5 must equal V1.
Model printed: per operand at np primes, now np x V0, raw-once V1 + (np-1) V2 + V3; RATIO4 = their ratio at np = 4.
Run-level model used by the driver's stop rule: saving = 174 calls x 36.5 ms x (1 - RATIO4) (calls and ms: O7b, m; assumes every gather call is covered, i.e. DIST_GATHER_RAW=2 and np = 4 throughout, as). Continue to the gates and the ABBA only if that is >= 1.5 s.

#### 3. Implementation (rns_dist.c, dist_core only: the single-node C-form product; the multi-node tier is untouched, its k_gather_mn24_np already reads once for all planes)
- `k_gather_raw<QS>` = k_gather without `ec_canon64`; `k_canon_pass` = elementwise `ec_canon64` plane -> plane; `gather_raw_all()` gathers raw into the last plane and canonicalises all np planes from it (the last in place, last).
- `DIST_GATHER_RAW=1`: A (miss) through its own xa[np-1]; B (miss) through its cache plane cb when it has one (and cb != ca). **No extra memory.** B without a cache plane: per prime as before.
- `DIST_GATHER_RAW=2`: additionally B without a cache plane through one extra plane of q limbs (4.29 GB per APU at 2^29 points, mod) taken from the block pool's **free extents only** (`db_pool_fit_count >= 1`, no growth, no hipMalloc) and only if `DIST_GATHER_RAW_MARGIN_GB` (default 2) stays free afterwards; otherwise B per prime (one note printed). Freed after the prime loop.
- Digits: canon(raw limb) is exactly the old gather's value, zero beyond the operand stays zero; identical by construction. Gates below check it.
- Memory and the margin rule: the extra plane is drawn from memory that is already mapped (the arena / pool sized at init), so the device total cannot grow through it; the rule "rank-0 margin >= 10 GB at the target size" (L1: 12.9 GB at 3.82e13 with VSLOT_RELEASE, mod) concerns that total and is unaffected by construction. What can change is the pool's peak live (up to +4.29 GB per APU) and, if another concurrent allocation then misses the free extents, pool growth by hipMalloc inside a phase: the driver reports device in use, pool peak live and pool hipMalloc per arm. At the target the products that use dist_core are the per-node ones below the grid tier (as); level 1 needs no memory at all.
- k_bload (B form, 1.2 s per APU) is not changed (O7b: no k_gather / k_bload overlap in one product; a shared raw staging for it is follow-up work).
- Switch rows: ecalc/README.md (one row), registry (switches_manual.tsv, regenerated switches.tsv / switches_gen.h; the regeneration also refreshed 4 stale rows from main: BI_MUL1_FAST, NEWTON_RECIP_CUT, NEWTON_RECIP_MID removed as having no code site, COMM_HS_TS added, COMM_LAYER_VSLOT_RELEASE scope; `--check` 0 problems).

#### 4. Driver and gates (tools/k1b_driver.sh, armed on aac6)
- Own hold: `sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J k1b` (no -w), queued behind our other jobs; give-up 12:00 Eastern (cancels only its own id).
- Step 1 probe (both shapes) -> model -> stop if < 1.5 s ("SUCCESS: refuted ...").
- Step 2 `tools/gate.sh --build --nodes 1 --hold J --env "RNS_STRATEGY=C DIST_GATHER_RAW=2"`, then without --build for =1 and =0 (1e9 digits against the 1e9 reference; RNS_STRATEGY=C because at 1e9 every product is the B form by default (O7b) and the gate would not reach the changed code otherwise).
- Step 3 ABBA at 6.441e10, A = DIST_GATHER_RAW=0, B = 2, no digit output, archive:drivers/s50/s50_abba6.sh (rounds ABBA / BAAB, paired t on round deltas, stop p<0.05 after >= 3 rounds, cap 5, or the hold's end); every run must print VERIFY OK.
- Step 4 memory per arm from the runs' `mem_report` lines; summary, marker, scancel of its own job, `squeue -s` shows no step.
- Dry run with stub sbatch / srun / squeue / gate (both the "confirmed" and the "refuted" path): summary, marker, scancel of the own id only: OK.

#### 5. Results
Collected from the aac6 summary (hold, 2026-10-12 03:03-03:19 ET; all numbers measured unless marked; marker `DONE` = SUCCESS).

- Probe (`t_gather_raw_probe`, two sizes): RATIO4 0.3388 (second size 0.3408); V1 raw gather 34.26 ms vs V0 gather+canon 34.45 ms (RAWREL 0.9944), canon pass alone 3.01 ms (CANONREL 0.0873), pow2 index 0.9991, local source 0.9933; all variants identical. Modelled saving at 6.441e10: 4.20 s per run (modelled, all gather calls covered, np 4).
- Gates (1 node, 1e9, RNS_STRATEGY=C): `DIST_GATHER_RAW=2`, `=1`, `=0` all PASS (VERIFY OK, digits identical, segv 0; ecalc total 11.24 / 15.02 / 11.86 s, single runs, not a timing comparison).
- ABBA, A = `DIST_GATHER_RAW=0`, B = `DIST_GATHER_RAW=2`, 6.441e10 digits, total seconds, 3 rounds, stop rule p<0.05 met:

| round | A (s) | B (s) | B-A (s) |
|---|---|---|---|
| 1 | 73.06 / 72.45 | 69.41 / 69.38 | -3.36 |
| 2 | 72.36 / 74.76 | 68.46 / 69.88 | -4.39 |
| 3 | 73.10 / 73.36 | 70.05 / 69.08 | -3.66 |

Mean B-A = **-3.805 s**, 95 % CI half-width 1.314 s (CI -5.12 .. -2.49), p<0.05 (significant; n = 3 pairs). All 12 runs VERIFY OK. Measured gain ~3.8 s of ~73 s (5.2 %), close to the 4.20 s modelled.
- Device memory (node, 4 APUs): in use max 284.1 GB both arms; block-pool peak live max 145.5 GB (A) vs 162.7 GB (B) (+17.2 GB); pool hipMalloc 0.0 GB; no "does not fit" notes.
- Verdict: significant gain, digits identical; adoption is the user's decision (cost: +17 GB peak live per node, within the 284 GB in use).

#### 6. K1c follow-up (2026-10-12): memory margin, multi-node relevance, 2-node aac7 item
Labels: **m** measured, **mod** modelled, **as** assumed.

**6.1 Does `=2` raise the device total the margin rule measures? No (m at 1 node, mod at the target).**
- aac6 ABBA logs (`~/k1b/abba/log/`, 12 runs): `device 284.1 GB in use` at every phase in both arms; block pool 180.4 GB, all of it borrowed from the init regions, `hipMalloc 0.0` in every run; no "does not fit" note (m). What moves is the pool's peak live, inside memory that is already mapped: bs 100.2 -> 117.4 GB, division 145.5 -> 162.7 GB per node (+17.2 GB = 4 x 4.29 GB, one raw plane per APU) (m). `=1` uses no extra memory at all (by construction).
- The margin rule (rank-0 device + 0.5 + 2.1 GB <= 373.44 - 10 GB) is computed from the layout's device total (`device_with`, L1/L4), i.e. planes + regions + pools as mapped, plus the comm pools; peak live inside the pool is not a term of it. The raw plane is taken only from free extents (`db_pool_fit_count >= 1`, 2 GB left free), never by growth, so the total cannot change unless the pool grows by hipMalloc, which the 1-node runs show it does not (m).
- Multi-node leaf (where the code runs at the target, 6.2): the 2-node aac7 run at 6.441e10 per node (`~/m2a/a/log/r11_n2.log`, main, m) has block pool 53.69 GB per APU, leaf mdev window peak 25.1-25.8 GB per APU, tree level 1 window peak 43.4-46.4 GB per APU. The raw plane (4.29 GB, freed after the prime loop) fits the leaf's free extents (about 28 GB per APU free, 6.3 GB needed with the 2 GB guard) and lifts the leaf peak to about 30 GB, still below the tree's 46 GB: the pool's whole-run peak is unchanged at multi-node (mod).
- Rank-0 margin at 576 nodes (mod, L1 / L4 method, T2048 + X2 + RELEASE): **3.71e13: 21.5 GB with `=0`, `=1` and `=2`; 3.82e13: 12.9 GB (L1; L4 13.0) with all three.** Condition: no pool growth by hipMalloc (the 2-node item reports `pool hipMalloc` and the largest APU's driver-used GB per arm). The 1-node division peak (+17.2 GB) does not carry over: at > 1 node the division is the mn tier (6.2).

**6.2 Does the changed code run in multi-node runs, and how much of the 576-node wall is it? Yes, in the per-node leaf only; about 1.4 s of ~176-182 s (mod).**
- dist_core is the single-node product (`mul_grid` -> `rns_mul_dist_db`). At > 1 node its callers are: the leaf's `mdev` levels on every node (binsplit.c, the top levels of the node's own subtree), and the reciprocal's single-node chain up to `NEWTON_MN_SPLIT` (2^16 limbs, negligible). The tree levels and the whole division at > 1 node go through `mn_grid` / `mn_core` (its own gather, `k_gather_mn24_np`, already one read for all planes): unchanged.
- 1-node phase split of the aac6 ABBA (6 runs per arm, m): leaf mdev 8.70 -> 7.35 s (**-1.35 s**), batch 19.67 -> 19.52 s (-0.15 s, noise level), dm 19.52 -> 17.07 s (-2.45 s, of which recip 4.77 -> 4.62 s). So about 1.35 s of the 3.8 s is in code that runs at 576 nodes; the dm part (2.45 s) is the single-node division, which a multi-node run does not use.
- Expected saving at 576 nodes (mod): the leaf is 6.441e10 digits per node at 3.71e13 (the measured size); aac7's leaf mdev is 7.2 / 8.8 s on node 0 / 1 of the 2-node run above (m); -15.5 % (the aac6 ratio) gives -1.1 / -1.4 s, and the tree starts after the slowest leaf: **about -1.4 s of 176.3 s (3.71e13) and 181.7 s (3.82e13, 6.63e10 per node, as same ratio), 0.7-0.8 %** (L4 model, fabric as). Assumes the gather's share of the leaf mdev on aac7's launch line (ECALC_NP=auto, RNS_DIST_CACHE_FIT/PARTIAL) equals aac6's plain launch; the cache planes may route more B operands through `=1`'s no-memory path.

**6.3 aac7 2-node item (armed 2026-10-12 04:27 Eastern)**
- `~/aac7q/queue/79_k1c_two.sh` (= `tools/k1c_item.sh`; NODES 2, EST_MIN 45, APPROVED yes): `tools/gate.sh --build --nodes 2 --env "DIST_GATHER_RAW=2"` (SHMEM build of k1b 30f6066 in `~/ntt-wt/k1b`, -DCOMM_SHMEM checked; a login-node build passed beforehand, libsma linked), then paired rounds (odd ABBA, even BAAB) A = `DIST_GATHER_RAW=0`, B = `=2`, 1.2882e11 digits on 2 nodes (6.441e10 per node), e16 aac7 line, no digit output, every run VERIFY OK; stop at p<0.05 after >= 3 rounds, cap 4 rounds or 40 min (3 rounds expected, about 34 min, as). Per run: total, leaf mdev (max over nodes; the phase the change acts on, the sharper measure), bs, dm, rank-0 device in use, pool peak live, pool hipMalloc, the largest APU's driver-used GB, not-fit notes.
- Dry run with STUB=1 (fake logs) locally and on aac7: summary, rounds, stop rule, marker OK.
- Queue: a 6-node item (72) was running at arm time; 79 starts when 2 nodes are free (expected 05:00-05:45, DONE about 05:35-06:20 Eastern, as).

**6.4 Result of the aac7 2-node item (collected 2026-10-12 05:14 Eastern; all numbers m)**
- Gate: `tools/gate.sh --build --nodes 2 --env "DIST_GATHER_RAW=2"` PASS: SHMEM build, VERIFY OK, 1e9 digits identical, segv 0, ecalc total 20.78 s.
- ABBA, A = `=0`, B = `=2`, 1.2882e11 digits on 2 nodes, 3 rounds (round 4 skipped by the 40 min budget), 0 FAIL, every run VERIFY OK:

| round | order | total A (s) | total B (s) | B-A total (s) | leaf mdev max A (s) | B (s) | B-A mdev (s) |
|---|---|---|---|---|---|---|---|
| 1 | ABBA | 133.00 | 132.47 | -0.52 | 7.20 | 6.10 | -1.10 |
| 2 | BAAB | 134.22 | 140.69 | +6.46 | 7.20 | 6.25 | -0.95 |
| 3 | ABBA | 142.51 | 138.03 | -4.48 | 7.15 | 6.25 | -0.90 |

- Total: mean +0.49 s, 95% CI half-width 13.76 s (t 0.15, crit 4.30): no effect resolved (the wall is dominated by bs 44-49 s and dm 54-68 s run-to-run noise; round 3 arm A had one 148.7 s run).
- Leaf mdev (max over nodes, the phase the change acts on): mean **-0.983 s, CI half-width 0.259 s** (t -16.4): significant, -13.7 % of 7.2 s. Matches the 6.2 model (-15.5 % of the leaf, 1.1-1.4 s).
- Memory, rank 0, max over 6 runs per arm: arm 0 and arm 2 are identical: device in use 317.8 GB, pool peak live 201.8 GB, **pool hipMalloc 0.0 GB**, largest APU driver-used 82.2 GB, not-fit notes 0. The raw plane fits the leaf's free extents; the 6.1 margin rule is unaffected at 2 nodes (the 576-node margins 21.5 / 12.9 GB stand, condition met).
- Verdict: `=2` saves about 1.0 s in the multi-node leaf with no memory cost, but the saving is below the noise of the 2-node total; no whole-run claim. Adoption is the user's call.

#### RESUME
- State: code + probe + driver on branch k1b (pushed). aac6 clone `~/ntt-k1b` (from a full bundle; the shared clone `~/ntt` is on the pre-rewrite history), built (`module load rocm; make -s -j16`, probe built). Driver armed 2026-10-12 02:32 Eastern (`setsid nohup`, GIVEUP_EPOCH 12:00 Eastern), hold job 22989 pending third in the PPAC queue behind two of our jobs; expected start ~03:05-03:50, DONE ~03:50-04:35 Eastern (as). Output `~/k1b/`.
- Collect: `ssh aac6 'cat ~/k1b/DONE; cat ~/k1b/summary.txt'` (one bounded command). Marker text: `SUCCESS: refuted by the probe ...` (write up: section 5 + verdict), `SUCCESS: probe ... gates PASS; DONE rounds=...` (copy the ABBA line, memory lines and probe numbers into section 5), or `FAILED: ...` (see summary.txt; probe.log, gate_*/, abba/log/).
- K1c (6.3) COLLECTED into 6.4 (done). Was: collect `ssh aac7 'cat ~/k1c/DONE; cat ~/k1c/summary.txt'` (marker `SUCCESS rounds=N` or `FAILED ...`; runs.tsv, rounds.tsv, log/, gate/); copy the ABBA lines and the two memory lines into 6.3. aac7 worktree `~/ntt-wt/k1b` (remove when the branch is decided).
- After collection: remove `~/ntt-k1b` and `~/k1b.bundle` on aac6 when the branch is decided; local worktree `../ntt-wt-k1b`.

## S50: k_gather coalesced write (DIST_GATHER_TILE, off by default) -- IMPLEMENTED, gated on CPX (test), timing queued (aac7 Job B queue, aac6 timers armed)

(former file: S50.md)

Branch s50 (from main daeb0f9). Status 2026-10-10 23:50 EDT: code + drivers done; probe, aac7 gates and ABBAs are queued/armed and have NOT run yet. All numbers labelled; none below is a speed result for the product.

#### 1. Design
`k_gather` (rns_dist.c) writes `x[il*C + j]` with `t -> (j = t / rows, il = t % rows)`: the source read is contiguous along il, the store is strided by C (S40: the next floor after the select-of-scalars change, which is already default as DBIG_QSEL=1). New kernel `k_gather_tile<QS,T>` (T = 16 / 32 / 64): a block of 256 threads owns a T x T tile (il x j); the read phase has lane = il (contiguous source run of T limbs, canon applied there), stores the tile in LDS `tile[T][T+1]`, the write phase has lane = j (contiguous destination row of T points). Grid = (rows/T)(C/T) blocks, il-block fastest. Same arithmetic per element, so digits cannot change; shapes with rows % T or C % T non-zero, and DBIG_QSEL=0, fall back to the old kernel.
Switch `DIST_GATHER_TILE` = 0 (default) / 16 / 32 / 64 (1 = 32); README row added. Two call sites (A and B planes of the single-node product) go through `gather_launch()`. The multi-node tier (k_gather_mn*) is untouched.
Micro-test: `tests/t_gather_probe.c` got three variants (tile 16 / 32 / 64, remote source, same plane compared with variant 0); build `make tests/t_gather_probe`; the S40 reference is the 4-way-select kernel (36.1 ms at rows 8192 x C 65536; 34.0 ms-class at rows 1024 x C 524288, measured S40).

#### 2. Evidence so far
| item | command | result | label |
|---|---|---|---|
| builds | aac7 `make -B SHMEM_CRAY=1 GMP_HOME=~/gmp` in ~/ntt-wt/s50 (log has -DCOMM_SHMEM); aac6 `module load rocm; make -s -j16` in ~/ntt-s50 | OK | measured |
| aac6 CPX s12-09 TEST run of the driver (23:34-23:42 EDT, 4e9 digits, POOL_LOG=29) | `s50_aac6.sh cpx` with S50_TILE=32 | gates 1e9: DIST_GATHER_TILE=0 and =32 both VERIFY OK, digits identical to e_1000000000.out; ABBA 4 complete rounds (+1 partial), all runs VERIFY OK; mean(B-A) = -0.005 s, 95 % CI half-width 0.69 s (n=4) -> no difference at the CPX scale | measured (driver test only; the real CPX run is armed for 03:30) |
| dry run of the aac7 ABBA driver | `DRY=1 s50_abba.sh` | stats/stop rule/marker logic OK | measured (stub) |
Expected gain (modelled, assumed): up to the S40 estimate for the strided write; unknown until the probe runs (the item only proceeds if a tile kernel is >= 3 % faster than the select kernel in the probe).

#### 3. Commands and drivers (all in archive:drivers/s50/)
- `s50_abba.sh <hold> <outdir> <NN 1|4> [digits/node]` -- aac7 ABBA inside a hold: A = `DIST_GATHER_TILE=0`, B = tile, env = headline LINE of e16_headline.sh + ECALC_SEGV_TRACE=1, mnrun.sh, 6.441e10 digits/node, no digit writes; round = A B B A / B A A B; stop at p < 0.05, CI half-width <= HALF_S, MAXR, or STOP_EPOCH (D); marker `<MARK>`.
- Job B queue items (copied to ~/batch1011/jobB/queue/): `20_1_probe_gate.sh` (EST 15 min: probe on the hold's first node -> `~/batch1011/T5/s50/TILE` (best tile or `skip`), then `tools/gate.sh --nodes 1 --hold $SLURM_JOB_ID` with DIST_GATHER_TILE=0 and =TILE, 1e9 digits; no rebuild here: the binary was built with the gate's own command), `20_2_engine.sh` (EST 30: `mnaccept.sh --only unit,e9,full` with the tile exported; log engine_gates.log), `20_3_abba1.sh` (EST 45, 1-node ABBA, <= 5 rounds), `20_4_abba4.sh` (EST 70, 4-node ABBA, <= 4 rounds). Items 2-4 exit immediately if TILE is `skip` (probe not faster, or the gate failed).
- aac6: `s50_aac6.sh cpx|ppac [START]` + `s50_abba6.sh`: own sbatch sleep job (<= 30-40 min, scancel of only that id), PPAC under `flock ~/batch1011/aac6_s24-26.lock`; ppac: probe -> TILE -> gates (own srun + digcmp, not tools/gate.sh) -> ABBA 6.441e10; cpx: TILE 32 (the probe needs 4 APUs), gates + ABBA at 4e9.

#### 4. Results (measured, aac7 Job B hold 12471, aac6 nodes; times Eastern)
**Probe** (aac7, `t_gather_probe`, 1 node, remote source; best of reps), rows 8192 x C 65536: 4-way select (S40, current default) 35.24 ms; tile 16 31.27 ms (0.89x); **tile 32 30.23 ms (0.86x)**; tile 64 35.72 ms (1.01x); all variants 0 mismatches vs variant 0. Second shape rows 1024 x C 524288: select 26.30 ms, tile 32 32.80 ms, tile 64 35.07 ms (tile 32 is slower than select there, 1.25x). The probe chose TILE=32 (>= 3 % faster at the first shape). The kernel is ~5 ms of one 35 ms launch; the product calls it twice.

**Gates** (tools/gate.sh style, 1 node, 1e9 digits, aac7): `DIST_GATHER_TILE=0` PASS (VERIFY OK, digits identical, segv 0, ecalc 11.65 s); `DIST_GATHER_TILE=32` PASS (same checks, ecalc 12.43 s; single runs, not a timing). aac6 CPX gates at 1e9 with 0 and 32 PASS (see section 2).

**ABBA, A = tile off, B = tile 32** (mean B-A, positive = tile slower; 95 % CI half-width):
| where | scale | rounds | mean B-A | CI half-width | result |
|---|---|---|---|---|---|
| aac7 1 node | 6.441e10 digits | 3 (stop: CI <= 1.5 s) | **-0.08 s** | 0.88 s | ns (deltas +0.3, -0.4, -0.2) |
| aac7 4 nodes | 6.441e10 digits/node | 4 (round cap) | **-0.07 s** | 30.6 s | ns (deltas +12.6, +0.8, +13.8, -27.6; the +-26 s bimodality of S47) |
| aac6 CPX (s12-12) | 4e9 | 5 (cap) | -0.046 s | 0.655 s | ns |
| aac6 PPAC slot 1 (s24-26) | 6.441e10 | 5 (cap) | +0.507 s | 0.591 s | ns (tile slower, point estimate) |
| aac6 PPAC slot 2 | 6.441e10 | 5 (cap) | -0.293 s | 1.029 s | ns |
The micro-gain of 5 ms x 2 launches (about 0.01 s) is two orders below the run noise; nothing could be seen, and none was seen.

**Engine gates** (`mnaccept --only unit,e9,full`, aac7 4-node hold, tile 32 exported): 7 passed, 4 failed: `t_bs` (VERIFY FAILED 2 of 10 checks), `t_mn_grid` (2 procs, timeout rc 124), and two `e9 size 1` (LIMB_BASE=10/2) whose reference file `~/ntt/ecalc/ref/e_1000000000.txt` does not exist on aac7 (the compare could not run: environment, not digits; the runs themselves are VERIFY OK). The t_bs and t_mn_grid failures were **not** compared against a baseline run without the tile (not done: out of time), so whether they pre-exist is unknown (doubt); neither unit uses the single-node k_gather path as far as the README states (not verified). The `full` step did not run (suite aborted at rc 4). This does not change the verdict (no gain to adopt), but it is the reason not to merge the code unreviewed.

#### 5. Verdict
- Do not adopt: no measurable gain (0.0 +- 0.9 s at 1 node; 4 nodes +-31 s; aac6 all ns), and the micro-benchmark shows the benefit depends on shape (tile 32 slower at rows 1024 x C 524288).
- Disposition (proposed, coordinator decides): keep `DIST_GATHER_TILE` code **off main**, preserved under an archive tag (`archive/s50-gather-tile`, set on the branch head); put results/EXCHANGE.md §S50 and the drivers (archive:drivers/s50/) on main.
- Not needed: any more aac7 time; a gain would need to appear as >= 0.5 s at 1 node to be detectable with 3 rounds.

#### RESUME
- Armed on aac6 (login-node timers, setsid): `s50_aac6.sh cpx` starts at epoch 1791703826 (03:30 EDT) -> marker `~/batch1011/T5/aac6_cpx/DONE`, summary `.../summary.txt`, pairs `.../abba/pairs.tsv`; `s50_aac6.sh ppac` slot 1 from 1791705026 (03:50 EDT, behind T5b's flock if it holds it) -> `~/batch1011/T5/aac6_ppac1/DONE`; slot 2 starts 3 min after slot 1 ends -> `~/batch1011/T5/aac6_ppac2/DONE`. Timer logs `~/batch1011/T5/{cpx,ppac}_timer.out`. Test run kept in `~/batch1011/T5/aac6_cpx_TEST`.
- aac7: items 20_1 .. 20_4 in the Job B queue (driver runs them after 10_ items; they are skipped if they would cross W 05:56 or D 08:56 EDT and retried after the handoff). Output dir `~/batch1011/T5/s50/`: `probe_item.log`, `TILE`, `gates.txt`, `gate_T*/gate.txt`, `abba1_*/ABBA1_DONE` + `stats.txt` + `pairs.tsv`, `abba4_*/ABBA4_DONE`; queue `.rc` / `.log` next to each item.
- Collect (one line, aac7): `cat ~/batch1011/T5/s50/TILE ~/batch1011/T5/s50/gates.txt ~/batch1011/T5/s50/abba*/ABBA*_DONE ~/batch1011/T5/s50/abba*/stats.txt 2>&1 | cut -c1-300`; aac6: `for d in cpx ppac1 ppac2; do cat ~/batch1011/T5/aac6_$d/DONE; done`.
- Not done yet: aac7 `tools/gate.sh --build` proper (the build used the same make command); the 4e10 step (mnaccept `full` runs 1e11 vs ~/ref instead, since e_4e10.out is gone from aac7); aac6 engine gates; the adoption proposal text after the numbers exist.
- Adoption rule: faster on every node tested + identical digits -> the coordinator proposes default (`gather_tile()` default in rns_dist.c); this track only proposes.

## P2: speed up k_gather -- STOPPED after a micro-probe: the kernel is bound by the remote read, and the best possible kernel-level gain is about 0.3-0.6 s per run (below what 4 ABBA rounds can resolve); no switch added

(former file: P2.md)

Labels: m measured, mod modelled. Times Eastern, 2026-10-12.

#### 1. Share of run time (from existing reports, m)
- 1 node, 6.441e10 digits, default DIST_GATHER_RAW=2: k_gather_raw<1> 2.0 s per APU (4.3 % of 47 s GPU time, results/SINGLE_NODE.md §CAND); the 58 raw gathers cost ~34 ms each, plus 232 coalesced canon passes ~3 ms each (mod ~0.7 s; results/EXCHANGE.md §K1b). Total ~2.7 s (mod) of ~70-80 s wall.
- 2+ nodes: the leaf's mdev uses the same dist_core path (results/EXCHANGE.md §K1b section on > 1 node: leaf mdev 7.2 / 8.8 s on aac7, gather saving at 3.8e13 ~1.4 s); the tree gathers (k_gather_mn24_np) have no kernel breakdown in any report; no profiled run was made.

#### 2. Probe (new tests/t_gather_p2_probe.c, aac7, 1 node, best of 8; rows 8192 x C 65536 / rows 1024 x C 524288; all variants identical to the product kernel, 0 mismatches) (m)
| variant | 8192x65536 ms | 1024x524288 ms |
|---|---|---|
| raw gather, strided write, remote source (today) | 35.4 | 21.2 |
| same, local source | 35.2 | 22.1 |
| tiled write (LDS 32x32), remote source | 30.4 | 32.0 |
| tiled write 32x32, local source | **3.8** | 3.9 |
| tiles 64x32 / 32x64 / 16x128 / 128x16, remote | 33.5-36.6 | 33.5-36.6 |
| read-only of the same limbs, remote source (coalesced reads, no big write) | **24.3** | 24.4 |
| kernel copy / DMA of 3 remote quarters (whole quarters, 4x the bytes the gather needs) | 142 / 220 | 142 / 220 |

Reading: the strided store is the bound only for a local source (35 -> 3.8 ms with a tile). With the striped (3/4 remote) source the floor is the remote read, 24 ms (about 130 GB/s of pull); the best tile (S50's 32x32) reaches 30 ms, no wider tile helps, and at the second shape every tile is slower than today's kernel. Staging remote quarters first by copy is slower (the gather needs a strided quarter of each operand, a full copy moves 4x more).

#### 3. Verdict
- Remaining kernel-level headroom: 35 -> ~25 ms per call x 58 calls = ~0.6 s per run (mod, upper bound; S50's tile gets ~0.3 s of it and measured nothing, -0.08 +- 0.88 s, results/EXCHANGE.md §S50). Four ABBA rounds resolve about +-1 s; a change cannot be shown. No code switch was added.
- Real lever (not a kernel tweak): avoid the 3/4 remote pull by layout, for example producing the operand already in the rank's rows, or moving the pull onto the multiplications' own all-to-all. That is a design change (ALGORITHM.md), proposed for the user's priority, not done here.
- Files: ecalc/tests/t_gather_p2_probe.c, Makefile rule (on demand build). Gates not run (no product code changed).

#### RESUME
Nothing running. Probe output on aac7 ~/p2/probe{1,2}.log; worktree ~/ntt-wt/p2 on aac7 to remove.

## K6: single-node C-tier all-to-all — overlap across primes (RNS_DIST_OVERLAP=1, off by default)

(former file: K6.md)

Branch k6 (from main a448d73, 2026-10-12). Source: docs OPPORTUNITIES §B.3 K6 ← results/KERNELS.md §S38 §2 ("the C-tier all-to-all: 10.0 s of
the 24.4 s C time is exposed host wait at 1 node; only deeper overlap across primes remains"). Labels: measured / modelled / assumed.
Times Eastern.

#### 1. What the 10.0 s is (code reading, measured inputs from S38)

`DIST_STATS` `t_a2a` is the **host** time blocked in `chunk_post` + `chunk_wait` (ntt_dist.c: the xGMI barrier, the push kernel's
stream sync, the barrier after it). With `DIST_CHUNKS=8` and depth 1 the host enqueues chunk k's row pass + pack on the compute stream
`s` and then blocks in `comm_wait` for chunk k−1's push on the transfer stream `ts`; that push itself waits (event `ev`) for chunk k−1's
row pass. So the host wait covers GPU compute that is on the critical path anyway — it is not GPU idle. The push (3 GB per plane
exchange per APU at 240 GB/s, ≈13 ms per transform at 2³¹ points, m) overlaps the next chunk's row pass (modelled 1.4 ms per chunk
against 1.6 ms of push). S38's "bandwidth lever is closed" stands; the measurable idle inside a transform is the four-APU barrier skew
(2 barriers per chunk, 192 per product) and the pipeline fill/drain at each of the 12 transform boundaries — small and not reachable
without restructuring the exchange.

What **is** serialised and not on the GPU's compute path: the **operand gathers**. For every prime p `dist_core` runs on `s`:
`k_gather(A → xa[p])`, `k_gather(B → xb)`, `hipStreamSynchronize`, then the three transforms and another sync. `k_gather` reads this
rank's rows of both operands from the four quarters by xGMI peer reads (3/4 remote) and reduces mod p — xGMI-read bound, few CUs, and
it re-reads the operands once per prime. S13 (measured, aac6, 2³¹ points, C form): load 0.35 s of 1.05 s per product (33 %); S38's
rocprof shows `k_gather` at 7.7 s per APU over the whole run (B and C products). Over the 29 C products' 24.4 s at 6.441e10 that is
about **8 s of gathers (modelled from the S13 ratio)** during which the transforms and the pushes idle, and vice versa.

#### 2. Design: the gathers of prime p+1 under the transforms of prime p

`RNS_DIST_OVERLAP=1` (rns_dist.c `dist_core`, the single-node C form only; the B form and the multi-node `mn_core` are untouched):
- a second stream `g` per rank (`rank_state.g`) runs every gather; `s` waits on the gather's event before transforming that prime;
- **A**: plane `xa[p+1]` is untouched by prime p, so A's gather of p+1 is enqueued on `g` when prime p starts (A's cache hit keeps
  its memcpy on `s`);
- **B**: one plane `xb` serves every prime. Two cases:
  - pool 0 has a spare plane (`np < 4` and the pool's cap ≥ (np+1)·q, the 4-prime sizing): B's planes alternate `xb` / the spare,
    and B's gather of p+1 is enqueued at the start of prime p (ordered after prime p−1's column inverse by an event on `s`);
  - otherwise a **hook** `dist_plan.after_cols` (ntt_dist.c `inv_pre_y`, and `dist3_inv` for the 3·2ᵏ planes) fires on the host right
    after prime p's column inverse — the last reader of `xb` (`ntt_inv_pw_y`) — is enqueued: it records an event on `s`, `g` waits for
    it and gathers B of p+1 into `xb` under the inverse's packs, exchanges and row passes;
- one `hipStreamSynchronize(s)` per product instead of one per prime (the inverse's tail and the next forward's head no longer drain);
- the gathers' device time is read from timing events (`RNS_VERBOSE` prints it as `load`, now overlapped; `ntt` is then the span).
Digits: the gathers, transforms, pointwise product and CRT run the same kernels on the same data; only their placement on streams
changes. Hazards checked: `xa[p+1]` has no reader in prime p; `xb` is written only after its last reader's event; the slab buffers
(`sbuf`/`rbuf`, the H1 `evr` wait) are not touched by the gathers; the cache fill (`ca`) stays on `s`.

Expected gain (modelled): up to the gather time of primes 1..np−1 (about 3/4 of the ≈8 s above = 6 s at 6.441e10) minus the xGMI
contention between the gathers' peer reads and the pushes (both use the links; the pushes occupy them ≈40 % of a transform, m) — the
ABBA decides. At 576 nodes the same code runs the node-local products (the leaf tiers, size-1 `dist_db` products); the distributed
products use `mn_core` (its gathers read local redistributed buffers) and are not changed.

#### 3. Files
- `ecalc/rns_dist.c`: `rank_state.g/ega/egb/ey`, `dist_overlap_on`, `gather_launch`, `struct ovl_st`, `ovl_gather_a/b`, `ovl_after_cols`,
  the prime loop of `dist_core`; `dist3_inv` takes the optional hook.
- `ecalc/ntt_dist.h/.c`: `dist_plan.after_cols/after_cols_arg` (0 by default; called once in `inv_pre_y` after the column pass).
- `ecalc/README.md`: the switch row. Drivers: `archive:drivers/k6/k6_aac6.sh` (aac6 slots), `k6_abba.sh` (paired ABBA, resumable),
  `86_k6_gate.sh` (aac7 queue item, 2-node gate).

#### 4. Tests (filled in by the driver's outputs; see RESUME)

| test | where | command | result |
|---|---|---|---|
| build | aac6 login node | `module load rocm; make -s -j16` in `~/ntt-k6/ecalc` | OK (measured, 2026-10-12 02:2x) |
| driver dry run | local, STUB=1 | `STUB=1 WT=<wt> O=<dir> MAXR=4 bash archive:drivers/k6/k6_aac6.sh` (stub runs, stop rule, resume after a dropped incomplete round) | OK |
| gate 1e9, switch on | aac6 PPAC 1 node | `tools/gate.sh --build --nodes 1 --hold $J --line none --env RNS_DIST_OVERLAP=1` | pending |
| gate 1e9, switch on, every product C | aac6 PPAC | `… --env "RNS_DIST_OVERLAP=1 RNS_STRATEGY=C"` | pending |
| t_strategy V (6 shapes, overlapped C vs B/B4/auto) | aac6 PPAC | `RNS_DIST_OVERLAP=1 ./tests/t_strategy 27 1 V` | pending |
| on/off digits at 4e9, every product C | aac6 PPAC | `RNS_STRATEGY=C [RNS_DIST_OVERLAP=1] ./ecalc 4000000000 <f>`, `cmp` | pending |
| ABBA 1e10 | aac6 PPAC | `k6_abba.sh` (A default, B `RNS_DIST_OVERLAP=1`; p<0.05 after ≥3 rounds, CI half ≤0.3 s, or 8 rounds) | pending |
| ABBA 6.441e10 | aac6 PPAC | the same, CI half ≤1.0 s | pending |
| gate 1e9, 2 nodes, switch on | aac7 (queue item 86) | `tools/gate.sh --build --nodes 2 --hold $J --line aac7 --env RNS_DIST_OVERLAP=1` | pending |

#### 4b. Collected aac7 gate (queue item 86, 2026-10-12 ~03:01 ET; measured)

2-node gate, 1e9 digits, `RNS_DIST_OVERLAP=1`, line aac7: **PASS** (rc 0, VERIFY OK, digits identical, segv 0, ecalc total 19.05 s, run 24 s). Marker `GATE2_DONE`, item rc 0.
This is a correctness gate for the 2-node path with the switch on; the single total is not a timing comparison. The aac6 gates and the 1e10 / 6.441e10 ABBA of the table above are separate and not collected here (still pending in section 4).

#### RESUME
- aac6 driver: `~/k6/driver.out`, summary `~/k6/summary.txt`, marker `~/k6/K6_DONE` (SUCCESS / PARTIAL / FAILED). Collect:
  `ssh aac6 'cat ~/k6/K6_DONE; cat ~/k6/stage/STAGE; cat ~/k6/abba10/stats.txt ~/k6/abba64/stats.txt'`. Per-run rows in
  `~/k6/abba{10,64}/pairs.tsv` (total) and `ctier.tsv` (C products, C seconds). The driver cancels its own holds (`-J k6`); it stops at
  its deadline (12 h after arming) with PARTIAL if the 6.441e10 rule is not met.
- aac7 queue item `~/aac7q/queue/86_k6_gate.sh` → `~/k6/GATE2_DONE`, `~/k6/gate2.txt` on aac7 (needs the queue daemon armed).
- Then: fill §4 and §5 (mean B−A with 95 % CI, C-tier seconds), decide (adopt as default only if significant and every run VERIFY OK),
  remove `~/ntt-k6` on aac6 and `~/ntt-wt/k6` on aac7 after merge.

#### 5. Collected aac6 results (2026-10-12; all numbers measured; marker `K6_DONE`: SUCCESS)

- Stage: `cmp4e9` PASS (1 777 782 789 bytes identical, `RNS_STRATEGY=C`, off vs on, both VERIFY OK); gate 1e9 on: PASS (ecalc total 11.30 s); gate 1e9 on, strategy C: PASS (12.36 s); `t_strategy` rc 0.
- ABBA (A = off, B = `RNS_DIST_OVERLAP=1`), total seconds:

| size | rounds | stop | mean B-A (s) | 95 % CI half (s) | significant | deltas (s) | arm A mean |
|---|---|---|---|---|---|---|---|
| 1e10 | 8 | round cap | -0.32 | 0.55 | no | -1.0, 0.8, 0.3, -0.5, -0.5, 0.2, -0.9, -0.9 | 24.35 s |
| 6.441e10 | 3 | CI half <= 1.0 s | -0.11 | 0.15 | no | -0.1, -0.1, -0.2 | 73.41 s |

C-tier seconds: 1e10 mean diff 0.000 (0 products, strategy C not used at that size); 6.441e10 mean diff +0.242 s, CI half 0.658 s, not significant (29 C products, arm A C-tier 20.68 s).
- Reading: the switch is correct (digits identical) but there is no measurable gain: -0.11 s +/- 0.15 s of 73 s at 6.441e10 (0.15 %), CI includes zero in the sense of p>0.05 at 1e10 and the C-tier time is not reduced. Verdict: do not adopt. 6.441e10 finished (not pending); the driver is gone and holds nothing.
