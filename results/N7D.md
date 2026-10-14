# N7D — SHMEM proxy mode toward comm_ofi speed at 6 nodes (copy kernel, parallel init, put-with-signal sends)

Scope: the N7D record (2026-10-14, aac7, 1, 2 and 6 nodes): where config C's 6-node gap went, three new switches of the proxy mode
(`COMM_SHMEM_PX_KCOPY`, `COMM_SHMEM_PX_INIT`, `COMM_SHMEM_PX_SEND`, all off by default), their gates and the 6-node ABBA against `comm_ofi`.
Dated record; the code and the central docs win where they differ. Labels: m = measured, mod = modelled, a = assumed.

**Question.** Can the proxy mode (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`, results/N7B.md, results/N7C.md) reach `comm_ofi` speed at 6 nodes, and
which fixes carry it? Baseline (results/N7C.md, m): config C (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 HSA_ENABLE_SDMA=0`)
458.7 s vs comm_ofi 216.3 s at 6 n, 3.8646e11 digits: +242.4 s, 95 % CI [+127.1, +357.7], 4 pairs.

**Diagnosis (L1, from the N7C logs and one 6-node diagnosis item, m).** The gap was not on the wire: level 1 of the tree (1 peer) was at parity,
the proxies were busy 17 % of the run, and t_comm's 5-peer exchange ran at 0.75 of comm_ofi. Inside ecalc the GPU work on the proxy pool
(`/dev/shm`, 4 KiB pages) was slow and uneven between nodes (block transposes 13-138 s per APU vs 2-4 s under comm_ofi), and ecalc's lock-step
spread one slow node to all. Init cost +17 s per pair (mesh creation with 5 PEs per node, serial pool registration, skew). Run-to-run spread was
large (config C: 385-1588 s).

**Change / branch.** Branch `n7d` = `n7d-copy` + `n7d-init` + `n7d-f4` merged, plus two fixes. All switches default off (digits unchanged).
- `COMM_SHMEM_PX_KCOPY=1` (`ecalc/comm_copy.c`): copies that touch a proxy pool run as a GPU copy kernel instead of on the SDMA engine;
  replaces the process-wide `HSA_ENABLE_SDMA=0`. Fix: `comm.h` declares `comm_copy_async` only outside `COMM_HOST_ONLY` (the host-only
  `tests/t_comm` build failed on `hipError_t`).
- `COMM_SHMEM_PX_INIT=1` (+ `COMM_SHMEM_PX_INIT_THREADS`): every PE pre-faults its pool (`MADV_POPULATE_WRITE`) and the node-process registers
  the four proxy pools and its own in parallel. Fix: the `madvise` result is checked; on an error (printed under `COMM_SHMEM_VERBOSE=1`) the pages
  are touched by an atomic no-op instead. At 6 n the pre-fault of a 2.5 GiB proxy pool took 0.08-0.17 s with no error (m): it works.
- `COMM_SHMEM_PX_SEND=2`: a proxy sends 4 MiB `shmem_ctx_putmem_signal_nbi` chunks straight into the receiving proxy's pool, chunks of the
  queued commands interleaved, one context per peer node retired by its own quiet (no batch quiet, no second hop for the signal).

**Method.** One aac7 queue item (`22_n7d_t1`, 6 nodes, 12:59-14:12 ET, commit 61400d7; node screen: nothing excluded). B = `COMM_OFI=0
COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 COMM_SHMEM_PX_KCOPY=1 COMM_SHMEM_PX_INIT=1 COMM_SHMEM_PX_SEND=2 COMM_LAYER_VSLOT_POOL=0
COMM_LAYER_INTER2=0` (no `HSA_ENABLE_SDMA`). `tools/gate.sh --nodes 1, 2, 6` with B; 1e10 write + compare at 2 and 6 n with B; a 2-node KCOPY
pair (1e10, no writes, order C K K C); `t_comm --bw 4 5 256` at 6 n; 6-node ABBA at 3.8646e11, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1
COMM_LAYER_STATS=1` on every run, A = comm_ofi default; one-factor runs. Earlier 2-node items: `20_n7d_f4`, `21_n7d_init` (branch tests).

**Results.**

Correctness (m): gates 1, 2 and 6 nodes with B: PASS, digits identical (ecalc 10.2, 19.4, 20.1 s). 1e10 write + compare with B: 2 n identical
(53.7 s), 6 n identical (41.9 s). Every completed run VERIFY OK; `/dev/shm` proxy files left per node 0 after every run that ended normally. Branch
tests (m): `COMM_SHMEM_PX_SEND=2` gates 1/2 n PASS + 1e10 identical (item 20); `COMM_SHMEM_PX_INIT=1` gates 1/2 n PASS + 1e10 identical (item 21).

2-node KCOPY pair, 1e10, no writes (m; 2 pairs, mirrored order):

| arm | total s | fabric stage s per APU thread (node 0) | exchange wait xw0 s |
|---|---|---|---|
| C (`HSA_ENABLE_SDMA=0`) | 28.46, 28.63 | 3.82, 3.60 | 14.5, 19.5 |
| K (`COMM_SHMEM_PX_KCOPY=1`) | 29.05, 26.71 | 3.41, 3.29 | 10.6, 13.5 |
| K − C, mean [95 % CI] | −0.66 [−16.6, +15.3] | −0.36 [−0.89, +0.18] | −4.95 [−18.3, +8.4] |

The copy kernel matches `HSA_ENABLE_SDMA=0` (fabric stage 0.36 s lower, inside the spread).

t_comm `--bw 4 5 256`, 6 n, all peers, GB/s per node (m; one run each, order C X X C; X = `COMM_SHMEM_PX_SEND=2`):

| slab | C (runs 1 / 4) | C + X (runs 2 / 3) |
|---|---|---|
| 4 MiB | 34.43 / 34.61 | 33.40 / 31.47 |
| 64 MiB | 34.13 / 30.50 | 33.25 / 31.67 |
| 256 MiB | 28.31 / 26.34 | 31.07 / 32.05 |

6-node ABBA, 3.8646e11 digits (m; 3 complete pairs, orders A B, B A, A B):

| round | A comm_ofi s | B s | B − A s |
|---|---|---|---|
| 1 | 192.91 | 292.20 | +99.3 |
| 2 | 184.49 | 247.78 | +63.3 |
| 3 | 197.37 | 284.38 | +87.0 |
| mean | 191.6 | 274.8 (sd 23.7) | **+83.2, 95 % CI [+37.7, +128.7]** |

Paired B − A per phase (m, 3 pairs): bs +37.7 s [+7.9, +67.5], dm +39.6 s [+24.9, +54.4], init +2.1 s [−2.0, +6.1], block transposes (max over
nodes) +0.13 s per APU [−0.33, +0.59] (A 2.4 s, B 2.6 s; was 13-138 s under config C), fabric stage node 0 +19.1 s [−0.8, +38.9].
The gap to comm_ofi fell from +242.4 s (results/N7C.md) to +83.2 s: B is 1.43 × comm_ofi (config C was 2.1 ×); B's spread fell from 385-1588 s to
248-292 s. Init is at parity with comm_ofi.

One-factor runs, 6 n, 3.8646e11 (m, one run each, no claim):

| run | total s | bs | dm | init | block transposes max s per APU |
|---|---|---|---|---|---|
| B (mean of 3) | 274.8 | 115.6 | 116.2 | 35.7 | 2.6 |
| B without `COMM_LAYER_VSLOT_POOL=0 COMM_LAYER_INTER2=0` | 244.4 | 97.8 | 105.3 | 35.2 | 4.4 |
| B with `COMM_SHMEM_PX_SEND=1` | 318.1 | 142.3 | 124.2 | 45.3 | 7.0 |

With KCOPY + NUMA + PX_INIT the v-slots in the pool cost little (transposes 4.4 s): the LAYER switches showed no gain (one run inside B's range).
`PX_SEND=2` may help (318 vs 248-292 s), one run.

Round 4 did not complete: during R4's B run a node failed (Slurm: node failure, the node then down, "Not responding", 13:56 ET) and the following
comm_ofi run timed out on the broken allocation; the item stopped there (its rule: any timeout). No digit check failed. Cause unknown
(site-side; the pre-fault adds 4 x 2.5 GiB per node of host pages, which the plan already counts).

**Verdict.** Merge the three switches, default off (gates and digits pass). The proxy mode is still slower than comm_ofi at 6 n (+83 s, CI excludes
0): comm_ofi stays the default. The copy kernel equals `HSA_ENABLE_SDMA=0`, so config C can be expressed without a process-wide environment setting
(D31 (b)). The LAYER switches are not needed.

**Open issues.** The remaining +83 s is in bs and dm exchange waits (xw0 +260 s summed over phases of pe 0, m), not in transposes or init; PX_SEND=2
vs 1 and LAYER on/off need paired runs; the node failure in R4 is unexplained; 4 KiB pool pages remain (admin change).

**RESUME.** `n7d` merged to main (switches off). Raw logs on the cluster (queue item 22 output). Next: D31 (config C as the `COMM_OFI=0` default, its own gates), then
more pairs of B vs comm_ofi if the user wants a decision on the proxy mode.
