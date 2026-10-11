# XEFF: why ecalc's exchanges underuse the transport, and what to change

Design study, 2026-10-08. No code changes, no runs. Read-only inputs: aac7 `~/s22/log/scale_n10_r1.log` (S22, 10 nodes, the
`MN_WAIT_STATS` / `MN_COMM_MARK` build) and `~/s19a/log/p10.log` (S19A, 10 nodes, `COMM_LAYER_STATS=1`). Also results/S19B.md, S20.md,
S22.md, OFI17.md, TUNE17.md, NIC16_experiments.md, I.md, RESULTS §112 and §116–§122. Code: `ecalc/comm_ofi.c`, `comm_shmem.c`,
`comm_layered.c`, `rns_dist.c` (`mn_core`, `redistribute`, `gen_fwd`, `gen_inv_pw`, `mn_round_add`), `ntt_dist.c`, `mn.c` at
`main` f09e2de. Models: `ecalc/estimate.py`.
Labels: **(m)** measured, **(d)** derived by arithmetic from measured numbers, **(mod)** modelled, **(a)** assumed.

## 0. Summary

- **Most of the time is spent on bytes the transport could move faster.** Per APU thread at 10 nodes (S19A p10, node 0), the SHMEM
  exchanges take 276.7 s post-to-completion for 1675 GB, an average of **6.05 GB/s** (m). That equals the model's fitted effective rate
  (`aac7_s18`, B = 6). The time divides into two populations:

| population | bytes / APU thread | time / APU thread | rate | what it is |
|---|---|---|---|---|
| layered transform exchanges (inter stage) | 1414.5 GB (84 %) (m) | 140.3 s (m) | **10.1 GB/s** (m) | the 3·np all-to-alls per product; back to back on the wire ("held" 84–92 s per node, m) |
| direct mesh-d exchanges (operand redistribution, result out, spills, `mdb_add_shifted`) | ≈ 261 GB (16 %) (d) | ≈ 136 s (d) | **≈ 1.9 GB/s** (d) | blocking `alltoallv` + immediate `wait`, one sync point each, ≈ 1176 per thread (d) |

- **Two gaps, of similar size:**
  1. **Direct exchanges (≈ 110 s per thread above the layered rate, d).** They carry 16 % of the bytes and take 49 % of the exchange
     time. Each one is a group-wide sync point. It absorbs the skew of the work before it, plus per-exchange fixed costs and the lockstep
     rounds. Skew is bounded at ≤ 68 s per thread at 10 nodes (S22 node spread, m). The split between skew and protocol is **not
     measured**.
  2. **Layered exchanges (≈ 80 s per thread, mod).** They run at 10.1 GB/s against 23.3 GB/s raw per NIC (NIC16, m). Decomposition
     per GB per APU thread (mod, §1.2):

| step | ms per GB | effective rate |
|---|---|---|
| raw NIC | 43 | 23.3 GB/s |
| + comm_ofi's per-peer protocol (delivery-complete writes, drain and handshake per exchange) | +14 | 17.5 GB/s |
| + 9-peer contention (every sender walks its peers in the same order) | +26 | 12 GB/s |
| + per-exchange tail and lockstep rounds inside ecalc | +14 | 10.1 GB/s |

- **Staging copies are not the problem.** Device-to-device copies into the fine-grained comm pool run at ≈ 1.6 TB/s, and kernels on
  fine-grained memory run at full HBM speed (I.md, m). The two staging copies cost ≈ 1.3 ms per GB, about 1.4 % of the wire time (d).
- **The ranked plan:**
  1. A stats-only switch, `COMM_XSTATS=1` (§3), to split the direct-exchange time into skew and protocol per call site and per APU
     thread.
  2. **Rotated, ready-first peer order** in comm_shmem (`COMM_SHMEM_PEER_ORDER=rot`). Small change, low risk.
  3. **Two inter exchanges in flight per mesh, with unstaged v-slots** (`COMM_LAYER_INTER2=1` + `COMM_LAYER_VSLOT_POOL=1`). Medium
     effort, memory-neutral.

  Items 2 and 3 together: about −30…−45 s at 10 nodes (mod, central). At 576 nodes: −60…−95 s on an aac7-class fabric, −8…−12 s on the
  target's standing estimate, and possibly 15–40 s more there from per-exchange fixed costs, which do not shrink with bandwidth (a; §2).

## 1. Where the gap comes from (10 nodes unless stated)

### 1.1 Message sizes, counts, rounds (m unless marked)

S22 `scale_n10_r1` (config A: `DM_MN_LEAN=1`, DC8, T1024; with stats). Comm-mark totals are node sums; "per thread" = /4 (a: four APU
threads).

| phase (g) | exchanges (node) | GB received (node) | MB per exchange | MB per peer | post→completion s per thread | GB/s per thread | phase wall s |
|---|---|---|---|---|---|---|---|
| tree level 1 (g = 2, power of two) | 2208 | 583.6 | 264 | 264 | 13.6 | **10.75** | 86.7 |
| tree level 2 (g = 10, general map) | 5308 | 1978.6 | 373 | 41 | 76.7 | 6.45 | 112.5 |
| reciprocal (g = 10) | 9400 | 1078.3 | 115 | 12.7 | 64.0 | **4.21** | 78.7 |
| division (g = 10) | 8312 | 2790.7 | 336 | 37 | 143.5 | 4.86 | 170.8 |
| whole run, pe 0 | 25228 | 6431 | 255 | | 297.8 | 5.40 | total 456.3 |

- **Every exchange takes the rounds path** (`COMM_SHMEM_ROUND_MB=1024` is on the line). All 25228 go through it. 580–668 per node need
  more than one round (3392–3948 rounds in all). The round chunk is 1024 MiB / (n − 1) per pair: 114 MB at g = 10, but **1.78 MB at
  g = 576** (d), where every pair above that is cut into lockstep rounds over 575 peers.
- **Sizes per layered exchange** (S19A layer-stats, node 0): 88 % of the layered time sits in the ≥ 537 MB/APU bucket at
  **10.26 GB/s**, and the 33–270 MB buckets run at 10.9–11.3 GB/s. The buckets below 17 MB run at 0.06–3.5 GB/s but sum to only
  ≈ 4 s of 149 s (m).
- **The per-exchange rate falls with g at the same bytes per node:** 10.75 GB/s at g = 2, 4.2–6.5 at g = 10 (m). At g = 10 the direct
  exchanges are a larger share, and the reciprocal is many small products.

### 1.2 The transport itself (comm_ofi under comm_shmem)

| evidence | value | label |
|---|---|---|
| one NIC streaming `fi_write`, 1 MiB, window 64 (nicbw); Cray SHMEM 4 PEs per node | 23.3 GB/s per NIC; 23.2 | m (NIC16 #4, #21) |
| `t_comm --bw` all-to-all through comm_shmem + comm_ofi (host-only build: host pool, CPU `memcpy` of the self slab) | 7.7 / 12.3 / 11.0 GB/s per thread at 2 / 4 / 10 nodes | m (TUNE17, OFI17) |
| same, SHMEM path through one NIC (cxi0) at 2 nodes | 19.8 GB/s per node | m (OFI17) |
| chunk 1–8 MiB × window 16–128 | flat (40–48 GB/s per node) | m (TUNE17) |
| fit T = b/c + (n − 1)·b/w to the 2- and 4-node points (c: the self-slab memcpy) | w ≈ 17.5 GB/s, c ≈ 14 GB/s; at 10 nodes w ≈ 12 | mod |

- **Reading:** a single-peer stream through comm_ofi reaches about 17.5 GB/s, not 23.3 (mod). With 9 peers it drops to about 12 (mod).
  The knobs are flat, so the loss is in the pattern and protocol, not the window. The code points:
  - **Same peer order on every sender.** `push_peers` and `rounder` loop `r = 0 .. n−1` (comm_shmem.c `push_peers`, `rounder` "the
    sender's half"). All senders aim at receiver 0 first, then 1, and so on: a moving incast hot spot.
  - **Head-of-line blocking.** The sender `wait_ge`s each receiver's offset word **in that order** (`push_peers` with `spin_us < 0`,
    which is always the case for OFI: `start_push` always uses the helper thread). One late receiver stalls the posts to every later
    peer, although they are ready.
  - **Delivery-complete on every 4 MiB write** (`open_nic`: `FI_DELIVERY_COMPLETE`). The signal goes only after the peer's last write is
    delivery-complete (`ofi_flush`), then as a **Cray SHMEM put through the process's single PE NIC (cxi0)**. So the offset and signal
    words of all four meshes share APU 0's NIC with mesh 0's bulk data.
  - **One exchange at a time per communicator** (`s_alltoall`: "alltoall while one is pending" dies; comm_layered: "the inter transport
    takes one exchange at a time"). Each exchange ends with a drain (the last write's ack, the signal, the receiver's `arrive`, `quiet`).
    The next exchange's first write waits for the next offset handshake. Inside each exchange, the tail (waiting for the slowest pair) is
    idle egress.
- **The ecalc layered rate (10.1–10.3 GB/s) against the t_comm wire estimate (12) at 10 nodes:** about 14 ms per GB (mod). For the
  ≥ 537 MB exchanges: 72.6 ms each for 0.745 GB (d), against ≈ 62 ms at 12 GB/s, so **≈ 10.5 ms of fixed cost per exchange** (mod).
  This is the tail, the drain, the rounder thread, two `hipStreamSynchronize`s and the copies (≈ 1 ms).

### 1.3 Depth and overlap

- Layered depth 2: exchange k+1's intra (xGMI) stage and transposes run under k's wire. xGMI is 13.9 s per thread at 94.5 GB/s, of
  which 6.7 s is overlapped and 7.3 s exposed (m, S19A).
- **The inter stage is strictly serial per mesh.** "held 86.5–94.3 s" per node (m) means exchange k+1 is ready but waits for k. The
  fabric is busy 94.7 % of the span (m), but "busy" here means "an exchange is pending", which includes the tail and drain of §1.2.
- `COMM_ALLTOALLV_DEPTH > 2` cannot help without a second inter communicator: the transport takes one at a time (B11: depth 1 only at
  K = 1, m).

### 1.4 Pack / unpack copies and staging

- The path of one v-exchange byte has five HBM copies:
  1. reorder into `x0` (if not contiguous);
  2. xGMI push into `x1`;
  3. reorder into `x2`;
  4. staging copy-in to the comm pool, then the wire, then staging copy-out into `x3`;
  5. scatter into `rb`.
- At 1.5–3 TB/s (I.md, X13b, m) this costs ≈ 3 ms per GB against ≈ 97 ms per GB on the wire: **about 3 %** (d). The block transposes
  take 2.8 s per APU in all (m).
- **Staging is not a bandwidth cost but a structural one.** `COMM_SHMEM_ROUND_MB` exists only to bound the staging. It turns every big
  exchange into lockstep rounds (copy-in → wire → all signals → copy-out per round), and at g = 576 the rounds are 1.78 MB per pair
  (§1.1).

### 1.5 Skew (waiting for peers)

| evidence | value | label |
|---|---|---|
| S22 n = 10, node spread (max − min) of `comm_wait` per thread: tree / reciprocal / division | 14 / 26 / 28 s (Σ 68 s) | m |
| same at n ≤ 4 | 0–20 s | m |
| reciprocal's single-node chain: 2.28 s minimum, but 6.8–22.2 s typical | the excess is a wait | m (S19B §2) |
| the same ranks wait most in both arms (dm: ranks 0–2; bs: ranks 8–9) | structural | m (S20) |

- The S22 "ready" counter mixes the offset-word waits (a peer is not there: skew) with the signal waits (data in flight: transfer), so
  it cannot split them (S22 §2.1). **The direct exchanges absorb all of it**, because each one is the first sync point after local work:
  gathers, CRT, `mn_round_add`, or the reciprocal's single-node chain.

### 1.6 Serialization between APU threads, and NIC binding

- **Mesh imbalance:** on every node, the four APU threads' layered fabric time differs by **max/min 1.10–1.18** at identical bytes (m,
  S19A, all 10 nodes; e.g. node 0: 129.9 .. 153.6 s).
- The four threads are coupled at every v-exchange, through the intra (xGMI) stage and the count all-gather in `vtab_build`
  (`comm_allgather_host` over the intra communicator per exchange). So the node runs at its **slowest mesh's** pace: up to ≈ 15 % of
  the layered fabric time (≈ 20 s per thread) is the faster threads waiting (d; upper bound).
- Which APU index is slow is not printed. Suspect: mesh 0, whose NIC (cxi0) also carries every mesh's SHMEM control words (§1.2) (a).
- **NIC binding is correct for data on aac7:** comm_ofi takes the cxi device on each APU's NUMA node, one per APU (code `nic_list`).
  Only the control plane is pinned to one NIC per node (Cray OpenSHMEMX: one NIC per PE, B1).
- **One-thread windows:** under `MN_T_CHUNK_MB`, every T round does a result exchange, then a spill exchange, an `omp barrier`, and
  `mn_round_add` on **one** thread while the other three APU threads and the wire idle (`mn_core`). The 2048 gain (−42 s, m) is local
  by S20's evidence. Its mechanism is still unexplained, and this window is a candidate (a).

### 1.7 The direct exchanges in numbers (S19A p10, node 0; d)

- Count: comm_shmem 16492 (node) − layered 11788 (node) = 4704, i.e. ≈ 1176 per thread.
- Size and time: ≈ 222 MB average, ≈ 116 ms each, where the layered rate would take ≈ 22 ms. That leaves **≈ 94 ms per direct exchange**
  of skew plus fixed costs.
- What a product does per APU thread (`mn_core`):
  - three blocking redistributions (A, B, X), each preceded by its pack and `hipStreamSynchronize`;
  - Kt × (result `alltoallv` + spill `alltoallv` + barrier + one-thread add);
  - the carry scans (`node_carry_in`, 1 byte per node over mesh 0).
- None of them overlaps anything.

## 2. Ranked candidate changes

**Conventions:**
- S = seconds saved per APU thread of exchange time at 10 nodes (mod or a, as marked). At 10 nodes S ≈ wall seconds (the exchanges are
  exposed: S19B, S22); `estimate.py`'s own slope is 1.14·S.
- Translation to 576 nodes (mod, assuming the fraction of exchange time saved carries over): `estimate.py` gives
  wall = 176.1 + 3429/B (aac7-class) and 146.2 + 3422/B (target-m), fitted to `--bw 6/7/8/10` and `--bw 28/33/40/47`.
  - aac7-class at B = 6: Δ576 ≈ **2.07·S**.
  - Target standing estimate (B = 47): Δ576 ≈ **0.26·S**; derated (B = 28): **0.44·S**.
- **The proportional rule understates fixed-cost fixes on the target.** Per-exchange tails and handshakes do not shrink with bandwidth.
  At 576 there are ≈ 2.5–4 k exchanges per APU thread (mod, S19B message count / 575), so 5–10 ms per exchange (a) is 12–40 s on the
  target. Items marked **F** below attack that term.

| # | change (switch, off by default) | S at 10 nodes | 576, aac7-class | 576, target-m / derated | memory | effort | risk |
|---|---|---|---|---|---|---|---|
| **0** | **`COMM_XSTATS=1`**: per-exchange phase timers (measurement only; §3) | 0 | 0 | 0 | 0 | S (≈ 150 lines) | none (stats) |
| **1** | **`COMM_SHMEM_PEER_ORDER=rot`**: rotated peer order (peer i = (me + 1 + i) mod n) and ready-first posting (post to any peer whose offset word has arrived, poll the rest) in `push_peers` and `rounder`; signal and copy-out per peer as it arrives | **10–40 (central 20)** (a; recovers part of the 26 ms/GB contention term and the head-of-line blocking in skewed direct exchanges) | −20…−83 (central −41) | −3…−10 / −4…−18 (F: removes head-of-line blocking, which grows with peers) | 0 | **S** (≈ 60 lines, comm_shmem.c only) | **low**: per-pair sequence numbers already make the order free; data placement is by the receiver's offset, so digits are identical |
| **2** | **`COMM_LAYER_INTER2=1` + `COMM_LAYER_VSLOT_POOL=1`**: the v-slots (x2/x3) from the comm pool (`comm_sym_alloc` on the inter communicator: unstaged, no rounds); a second SHMEM communicator per mesh so v-exchange k+1's inter stage posts while k's tail completes ("posting k completes k−1" kept for the callers) | **10–25 (central 18)** (mod: the ≈ 10.5 ms per-exchange tail on ≈ 1824 large layered exchanges per thread, plus the lockstep rounds of v-exchanges) | −21…−52 | −3…−7 / −4…−11; **F**: also removes the 1.78 MB-per-pair lockstep rounds at g = 576 on the 192 and 576 levels (the largest, S19B) | **neutral**: the v-slot term (1.72 GB per APU at DC8, m) moves from `hipMalloc` into the comm pool; no new staging. Option: then `COMM_SHMEM_ROUND_MB=512` for the remaining direct exchanges, −1 GB per APU (mod) | **M** (comm_layered.c, mn.c ids, binsplit pool rule + mem_model `--check-c`) | medium: a new comm id range (trap B9), pool fragmentation (allocate the slots once at the layout's maximum) |
| 3 | `MN_REDIST_FUSE=1`: one `alltoallv` for A, B, X's redistribution (segments back to back per peer), instead of three blocking ones | 3–12 (a; 2 of 3 sync points per product, ≈ 120 per thread × the ≈ 94 ms excess, of which an unknown part is skew that just moves) | −6…−25 | −1…−3 / −1…−5 (F) | 0 (A's and B's receive buffers already coexist; the sends sum) | S–M (rns_dist.c `redistribute`, `mn_core`) | low–medium (P24 path, cache partial hits) |
| 4 | `COMM_OFI_SIG=1`: data writes transmit-complete; per peer, one 8-byte write with `FI_FENCE \| FI_DELIVERY_COMPLETE` into a signal area in the receiver's comm pool, on the device's own NIC; offset words likewise. This takes the control plane off cxi0 | 10–30 (a; the 14 ms/GB protocol term plus the mesh imbalance, if mesh 0 is the slow one) | −21…−62 | −3…−8 / −4…−13; **F at 576**: 575 × 2 control words per exchange per mesh leave the single PE NIC | 0 (+ a few KB of signal area per communicator) | M–L | medium–high (cxi `FI_FENCE` semantics, CPU polling of device memory, ordering proofs); **gated on M2** |
| 5 | `COMM_SHMEM_RPIPE=1`: rounds pipelined two deep (round j+1's offsets and writes go to ready peers while round j's slow pairs finish); staging split into two halves of the same budget | 5–15 (a; direct exchanges in more than one round: ≈ 165 per thread, ≈ 970 rounds) | −10…−31 | −1…−4 / −2…−7 (F) | 0 (same `ROUND_MB` budget) | M (comm_shmem.c `rounder`) | medium |
| 6 | Skew sources, not the transport: the reciprocal's single-node chain wait (≈ 10 s mean excess, S19B #4); `mn_round_add` on one thread | 5–15 (a) | n/a | n/a | 0 | S–M | low; diagnose with `ECALC_LOG_CLOCKS` and XSTATS first |

**Not recommended (evidence in hand):**
- `COMM_OFI_CHUNK_MB` / `WINDOW` tuning: flat (TUNE17, m).
- Two NICs per APU on aac7: no gain, one NIC per APU there (TUNE17, m).
- `DIST_MN_SYM_SLABS=1`: +3q of comm pool per APU (≈ 10 GB per APU at the cap, mod) to save copies that cost about 1 % (§1.4).
- `MN_TOPO_GROUP`: aborts under SHMEM and is inert (B9).
- More `DIST_CHUNKS`: 2× messages, time-neutral (S19B / S20, m).

**Order of work:**
1. 0 (`COMM_XSTATS`) and 1 (peer order) can be built together: 1 is independent of what 0 finds.
2. Run M2 (t_comm A/B, minutes) and then the 10-node ABBA of 1.
3. Build 2 next.
4. 3, 5 and 6 only if XSTATS shows their populations are large: redistribution sync time, multi-round direct exchanges, skew at the
   reciprocal's start.
5. 4 only if M2 shows delivery-complete costs more than 15 % on a single peer stream.

## 3. Measurements needed first (cheap)

### M1 `COMM_XSTATS=1` (stats only, off by default, digits unaffected; build in the same branch as item 1)

**Where:**
- `comm_shmem.c`: `s_alltoall`, `s_alltoallv`, `alltoallv_rounds` / `rounder`, `push_peers`, `ofi_flush`, `arrive`, `s_wait`.
- `comm_util.c` / `comm.h`: next to the D3 `comm_wst_*` phase machinery (reuse its phase: bs / dm / recip / other).

**Per exchange, record:**
- `t_post`: entry;
- `t_staged`: copy-in done;
- `roff_wait`: Σ blocked in `wait_ge` on `W_ROFF` words = **skew**;
- `t_lastpost`: the last `comm_ofi_write` returned;
- `t_flushed`: `ofi_flush` returned (my data delivered);
- `sig_wait`: Σ blocked in `wait_ge` on `W_SIG` words = **transfer and the tail**;
- `copyout`;
- `t_waitcall`: when the caller entered `s_wait` (so "caller late" is told from "wire busy");
- `t_end`.

Plus bytes, rounds K and peers.

**Tags:**
- The **kind** is a thread-local `comm_xtag` set by the callers before posting and captured into `shm_priv` at post time (the rounder
  runs in a helper thread):
  - `comm_layered.c` `inter_post` → LAYER_EQ, `v_post` → LAYER_V;
  - `rns_dist.c` `redistribute` → REDIST, the result `alltoallv` in `mn_core` → RESULT, the spill exchange → SPILL,
    `mdb_add_shifted` → ADDSH;
  - default OTHER.
- The **APU index** comes from `hipGetDevice` at post time.

**Output** (at each `MN_COMM_MARK` point and at exit, node 0 plus the max/min over nodes like `mn_wait_stats_print`): one line per
(phase, kind) with count, GB, the sums of each interval and K. One line per APU index with the inter-stage seconds and GB, to name the
slow mesh. Split the existing S22 `ready` counter into `ready_roff` / `ready_sig` at the same call sites (10 lines).

**Run:** one 10-node run on the aac7 base line + `COMM_XSTATS=1 MN_WAIT_STATS=1 MN_COMM_MARK=1` (one run, ≈ 8 min; compare shapes, not
levels: the stats cost 3–4 % in S22).

**Decides:**
- what share of the ≈ 110 s per thread of direct-exchange excess is skew (`roff_wait`) versus tail and protocol (`sig_wait`), by kind;
- whether the slow mesh is APU 0 (→ item 4);
- how many direct exchanges are multi-round (→ item 5).

### M2 t_comm transport A/B (≈ 15 min on the hold, no ecalc)

- `t_comm --bw 4 5 256` (sym) at 2, 4 and 10 nodes, `COMM_OFI_POOL_MB` sized for 10 peers (§117 lesson), ABAB × 3, with
  `COMM_SHMEM_PEER_ORDER` = 0 / rot.
- Two small additions to `t_comm`, test code only:
  - **`noself`**: skip the self-slab `memcpy` of the host-only build, which caps the 2-node figure (§1.2);
  - a **`COMM_OFI_DC=0` diagnostic** (transmit-complete writes, then one fenced delivery-complete 8-byte write per peer before the
    signal): t_comm VERIFY must pass.
- **Pass criteria:**
  - item 1 is worth an ecalc A/B if the 10-node per-thread rate rises ≥ 10 % (11.0 → ≥ 12.1 GB/s);
  - item 4 is worth building if DC=0 lifts the 2-node single-peer rate ≥ 15 %.

## 4. The A/B test for each built switch (exact)

**Setup:**
- Hold: 10 nodes, one network program per node, no other job on them.
- Driver: `tools/rundriver.sh`, like `archive/drivers/ecalc/s22_batch.sh`.
- Base line, the aac7 base of 2026-10-08:
```
COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1
MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=2048 DM_MN_LEAN=1 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6
ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 COMM_OFI_PLAN_CXI=1          (DIST_CHUNKS = code default 8)
SLURM_JOB_ID=$J MNRUN_NODES=10 ./mnrun.sh 10 env $LINE [$SW] ./ecalc 644100000000      # 6.441e10 digits/node, no write
```
- A = base, B = base + the switch (item 1: `COMM_SHMEM_PEER_ORDER=rot`; item 2:
  `COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1`; combined: both).
- **ABBA:** odd rounds A first, even rounds B first. Evict `~/ref/e_*` from the page cache before each run (S22's `evict`).
- **Stop rule:** S22's blocks of 4 rounds; stop at |t| > t_crit or 16 rounds (≈ 4.3 h at ≈ 8 min per run). S22's paired sd of 21 s
  gives a 95 % CI of ±11 s at 16 rounds (m), enough for the central 18–20 s.

**Per run, record:**
- `total`, `bs`, `dm`, recip, division;
- the four comm-mark lines;
- comm_shmem pe lines;
- the top-node peak (`MEM_REPORT_DEVS`);
- VERIFY.

**Before arming (gates, docs/AGENT_PROTOCOL.md):**
- `t_newton`, `t_mul`, `e9` identical in both bases, 4 × 10¹⁰ identical to `e_4e10.out`;
- `./mnaccept.sh $J --only unit,e9` with the switch on (layered self-test, general map sizes 3/6/10);
- `t_comm` VERIFY at 2/4/10 nodes with the switch on;
- for item 2: `mem_model.py --check-c` exact with the moved v-slot term, and `MN_PLAN_ONLY=37100000000000:576` with and without the
  switch: `plan pool` and the node / device totals must not grow.

**Adopt-or-not evidence:**
- paired B − A (mean, median, sign test);
- the per-phase comm-mark GB/s per thread (it should rise at g = 10);
- XSTATS (if on): `roff_wait` and `sig_wait` per kind;
- peak memory unchanged (item 1) or within ±0.5 GB per node of the moved term (item 2).

## 5. Implementation notes for the top three (for a sonnet agent)

### Item 0, `COMM_XSTATS`

As M1 above.
- **Files:** `comm.h` (the tag enum, `comm_xtag_set(int)` thread-local), `comm_util.c` (sums under the D3 phase), `comm_shmem.c`
  (timers), `comm_layered.c` and `rns_dist.c` (two to four `comm_xtag_set` calls each), `mn.c` (printing next to
  `mn_wait_stats_print`).
- **Off-path cost:** one flag test.
- **README:** one row for `COMM_XSTATS`.

### Item 1, `COMM_SHMEM_PEER_ORDER=rot` (`comm_shmem.c` only)

- Add `S.porder` (env, default 0) and `static int peer_at(const shm_priv *p, int i)` = `(p->me + 1 + i) % p->n` for i < n − 1.
- **`push_peers`:** when `S.porder`, keep a `char *pend` per call. Loop over the pending peers in rotated order with
  `test_ge(W_ROFF)`; `put_signalled` each ready one; after a pass with none ready, `sched_yield`. The `spin_us >= 0` inline variant
  returns the first not-ready index as today; it is never taken under OFI.
- **`rounder`, the sender's half of round j:** the same ready-first rotated loop.
- **`rounder` and `arrive`, the signal waits:** under the switch, poll `test_ge(W_SIG)` over the pending peers and start each peer's
  copy-out as soon as its signal arrives. Correctness only needs all of them before the `quiet`.
- **`ofi_flush`:** unchanged (it already signals per peer as soon as that peer's counter reaches 0).
- **Do not change:**
  - `put_word` / `publish` order (irrelevant);
  - the sequence arithmetic;
  - comm_ofi's NIC striping `j = (i + r) % k`.
- **Tests:** `t_comm` VERIFY with the switch at 2/4/10 nodes; `mnaccept unit,e9`; e9; 4e10; then M2 and the §4 A/B.

### Item 2, `COMM_LAYER_VSLOT_POOL=1` then `COMM_LAYER_INTER2=1`

- **VSLOT_POOL:**
  - In `need_vslot`, when the inter communicator gives `comm_sym_alloc` memory, allocate each slot **once** at the layout's maximum.
    `binsplit_vslot_bytes` / B7ACCT's `A + max(A, B) + 4` at the plan's largest q avoids grow-and-fragment in the first-fit pool.
  - Free with `comm_sym_free`.
  - In `comm_shmem.c` `alltoallv_rounds`: `if (sin && rin && <switch>) return 0;`, because rounds only bound staging.
  - Pool rule: add 2 slots to the comm pool's need per APU, and remove the same term from the `hipMalloc` v-slot accounting, in C
    (`binsplit.c`) and `mem_model.py`.
- **INTER2:**
  - `mn.c`: create a second SHMEM communicator per (level, d) from a fresh id range, checked against `MAXID` 1024; trap B9 reused
    `NA + NA·(2l+1) + d`.
  - `comm_layered.c`: hand it to the layered communicator as `p->inter2`. Exchange k uses `inter[k & 1]`. Turn `struct lay_v` into a
    ring of two pending entries.
  - In `y_alltoallv2`, the order becomes `v_stages(k)` (slot k & 1, free because k−2 completed), then `v_post(k)` on `inter[k & 1]`,
    then `complete_v(k−1)`. "Posting k completes k−1" stays true for `gen_fwd` / `gen_inv_pw`.
  - `complete_v` before any equal-slab exchange and in `y_wait` must drain both entries.
  - Require VSLOT_POOL (fatal otherwise: two staged inter exchanges would double the staging).
  - The equal-slab path (power-of-two g) can follow later in the same way.
- **Tests:**
  - `mn_selftest_layered` at sizes 3, 6, 10 (the general map) and 2, 4, 8;
  - `t_dist`;
  - `mnaccept unit,e9`;
  - e9;
  - 4e10;
  - `--check-c`;
  - `MN_PLAN_ONLY :576`.

## 6. Decisions for the user

| decision | why now | options | cost / benefit |
|---|---|---|---|
| X1: build items 0 + 1 (`COMM_XSTATS`, rotated ready-first order) | the stats decide which later items are worth building; item 1 is the cheapest transport fix and matters most at 576 (head-of-line blocking grows with peers) | (a) build both (one branch, S effort), then M1 + M2 + the 10-node ABBA; (b) item 0 only; (c) neither | (a) ≈ 1 day of agent work + ≈ 5 h of hold; −10…−40 s at 10 nodes (a), −20…−83 s at 576 aac7-class (mod); no memory; low risk. (b) data only (≈ 1 h of hold). (c) the 110 s per thread of direct-exchange time stays unexplained |
| X2: build item 2 (inter2 + unstaged v-slots) after X1's data | it is the only item that removes the lockstep rounds at g = 576 (1.78 MB per pair) | (a) build (M effort) and A/B; (b) wait for target wait-stats (TARGET_TASKS T12 `s2chk`) | (a) −10…−25 s at 10 nodes (mod), memory-neutral, medium risk (ids, pool layout); (b) no cost, but the 576 round structure stays untested |
| X3: item 4 (OFI-native signalling) | only if M2 shows the delivery-complete cost | (a) gate on M2; (b) build now | (a) avoids an M–L, higher-risk build if delivery-complete is cheap; (b) up to −30 s at 10 nodes (a), and the control plane leaves cxi0 at 576 |

Decisions X1–X3 are proposals. No switch here exists yet; every one would be off by default, and adoption is the user's decision on
measured data.

## Reproduce (bounded reads)

```
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
$R 'cd ~/s22/log; grep -E "^comm-mark|^wait-stats node 0|^total|^comm_shmem: pe [0-9]+: all-to-all" scale_n10_r1.log | cut -c1-400'
$R 'cd ~/s19a/log; grep -E "layer-stats.*node 0" p10.log | cut -c1-400; grep -E "NIC balance|one-at-a-time" p10.log | cut -c1-300'
cd ecalc; for b in 6 7 8 10; do python3 estimate.py --fabric aac7_s18 --bw $b --D 6.441e10 --g 10 576 | grep -E "^(10|576) "; done
for b in 28 33 40 47; do python3 estimate.py --fabric target-m --bw $b --D 6.441e10 --g 576 | grep -E "^576 "; done
```
Model values used (mod):

| model | B (GB/s per APU) | wall |
|---|---|---|
| aac7_s18, 10 nodes | 6 / 7 / 8 / 10 | 435.3 / 390.3 / 356.5 / 309.3 s |
| aac7_s18, 576 nodes | 6 / 7 / 8 / 10 | 747.6 / 666.1 / 605.0 / 519.0 s |
| target-m, 576 nodes | 28 / 33 / 40 / 47 | 268.4 / 249.9 / 231.7 / 219.0 s |

## X3 verdict (2026-10-08, M2 measured): item 4 (OFI-native signalling) is NOT worth building

**Test (measured):** `t_comm --bw 4 5 256 sym`, 2 nodes (x9000c1s5b0n0, x9000c1s6b0n0 of hold 12377), host-only t_comm build, comm_ofi on (cxi), base env of §4
(`COMM_SHMEM_DEVHEAP=1 COMM_OFI_PLAN_CXI=1`, pools 8192 MB), ABBA x3 = 6 runs per arm. Arm A = default (data writes `FI_DELIVERY_COMPLETE`); arm B = `COMM_OFI_DC=0`
(new diagnostic on branch s29, `comm_ofi.c`: transmit-complete data writes with no delivery wait and no replacement fence, i.e. an UPPER BOUND for item 4, unsafe: the SHMEM signal may pass the data).
Driver `archive/drivers/ecalc/s29_m2.sh`, branch s29; logs `~/s29m2/` on aac7.

| slab | DC=1 per-thread GB/s (6 runs) | DC=0 per-thread GB/s (6 runs) | lift |
|---|---|---|---|
| 256 MiB | 6.66 mean (6.14-7.63) | 6.67 mean (6.24-7.47) | +0.2 % |
| 64 MiB | 6.78 mean (6.42-7.14) | 6.79 mean (6.45-7.21) | +0.1 % |
| 16 MiB | 7.02 mean | 6.97 mean | -0.7 % |
| aggregate 4 threads, 256 MiB | 24.9 GB/s (mean) | 24.2 GB/s (mean) | within noise |

Run-to-run spread is about +-8 %; both arms scatter identically. `t_comm` VERIFY: OK on both PEs for DC=1 and also DC=0 (no data race observed, not a proof).

**Gate (§3 M2: item 4 worth building if DC=0 lifts the 2-node single-peer rate >= 15 %):** measured lift 0.2 % (an upper bound, since the real item 4 adds a fenced
8-byte write per peer). FAILS by two orders of magnitude. Delivery-complete costs nothing measurable on the bandwidth path; the 14 ms/GB "protocol term" is not the
delivery wait. Item 4's remaining claim is only F (control words leaving cxi0 at 576 PEs), which M2 does not test and which XSTATS (item 0) can show if it matters.
**Decision (per the gate rule): X3 not built; no s29 batch armed.** Switch `COMM_OFI_DC` (default 1 = unchanged) and `MNRUN_NODELIST` (mnrun.sh, default unset) remain on branch s29 only.
