# A-comm — M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19)

Branch `worktree-agent-a69986025b3273e8b` (from `main` at a75474d). PLAN.md §19, agent A-comm;
results/M3.md "Open issues" (the O(g) point-to-point all-gathers).

## Files

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

## Design

### The all-gathers

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

### The slab pipeline (`ntt_dist.c`)

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

### mnrun.sh

Unchanged; no launching problem was reported by the other agents during this session (their batches ran
`mnrun.sh` with 1–4 node-processes on one node alongside mine).

## Tests

Build in `~/ntt-acomm` on aac6 from the branch bundle. Batch 1: job 20710 (one node, s24-26); batch 2: job
(below). Every ecalc digit file `cmp`-identical to `ref/e_<digits>.txt`.

### Batch 1 (job 20710, one node)

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

### Batch 2 (job 20728, one node s24-30; the other two nodes were allocated throughout, so no real-node runs)

| test | command | result |
|---|---|---|
| synthetic four ranks, with the `s_wait` fix | `./tests/t_dist 26`, `DIST_TINV=1 ./tests/t_dist 26` | VERIFY OK (66 checks) both |
| TCP communicator, 3 and 8 ranks | `./tests/t_comm 3 27500; ./tests/t_comm 8 27600` | VERIFY OK both |
| four APUs to 2^31, K = 1 / 4 / 8 / 4 | `DIST_XGMI=1 DIST_CHUNKS=K ./tests/t_dist 31` | VERIFY OK (67 checks) each; timings below |

### The xGMI pipeline at 2^30 points (four APUs, `DIST_XGMI=1 ./tests/t_dist 31`, job 20710)

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

## Gate status

| item | status |
|---|---|
| t_comm, t_dist in DIST_XGMI=1, COMM_RANK (2, 4) and DIST_LAYERED=1 (2, 3, 4 node-processes) modes on one node, to 2^26 | OK |
| the same on 2–3 real nodes | **not run**: the other two nodes were allocated by other agents for the whole session (batch 2 checked `sinfo` and found one idle node). The layered code paths across hosts are M3's, verified there on 2–3 real nodes; what is new on the wire is the TCP receiver thread and the second exchange in flight, both host-side logic that does not depend on the hosts being distinct. To run when idle: `mnrun.sh 2/4 env DIST_LAYERED=1 ./tests/t_dist 26` and `mnrun.sh 2 env POOL_LOG=27 ./ecalc 100000000 out` on `-N2` (the script `~/acomm_batch2.sh` does it when two nodes are idle). |
| ecalc 10⁸ at sizes 2 and 4 identical | OK (one node) |
| the single-node 10⁹ bit-for-bit | OK |
| xgmi t_dist 31 timing not slower | OK: the transform is 2.5–8 % faster at 2^30–2^31; the whole run is equal to `main`'s (65.0 vs 63.4–64.7 s, host-bound) |

## Open issues

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

## Files touched outside my list

`ecalc/mn.c`: `mn_allgather`'s body only (→ `comm_allgather_host`), as allowed.
