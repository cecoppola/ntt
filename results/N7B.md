# N7B — one SHMEM proxy PE per APU (`COMM_SHMEM_PE_PER_APU=1`): bandwidth, gates, 6-node ABBA

Scope: the N7b record (2026-10-14, aac7, 1 to 6 nodes): the switch, its gates, `t_comm` bandwidth and the 6-node full-run
comparison against `comm_ofi` and SHMEM today. Dated record, frozen when written; the code and the central docs win where they differ.

**Question.** One SHMEM PE per node uses one of four NICs (vendor limit). Does one proxy PE per APU (each on its own NIC, the
node-process stays the compute rank) lift the SHMEM-only path (`COMM_OFI=0`) towards `comm_ofi`?

**Change / branch.** Branch `n7b`, merged to main 8acda73 (parameter `COMM_SHMEM_PE_PER_APU`, 0 = off = default, 1 = one proxy PE per
APU; needs `COMM_OFI=0` and more than one node). `ecalc/comm_shmem.c` (data plane in the `comm_ofi` mould: the proxies post the puts
from their own symmetric heaps; the node-process maps those heaps through named `/dev/shm` files, an `mmap` wrapper armed only inside a
proxy's `shmem_init`, because the on-node path of the aac7 library is absent), `ecalc/mnrun.sh` (launch of 1 + 4 PEs per node,
4 NICs, round-robin), `binsplit.c` (pool rule per PE). Design summary: logical PE = node (mn.c, meshes, callers unchanged); proxy d on
NIC d; control words stay on the node-process; value 2 (two NICs per APU, the target) designed, refused at init. Default unchanged.

**Method.**
- B2 (measured): 2 and 6 nodes, `t_comm --bw 4 5 256` (GB/s per node, 2 runs per mode in mirrored order), gates `tools/gate.sh` 1 and 2
  nodes at the defaults and with `--env "COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1"`, a 2-node 1e10 write + compare, two 2-node 1e10 no-write runs.
- B3 (measured): one queue item, 6 nodes, build of main b6ec90c: node screen (`--want-nics 4`, nothing excluded), 1e10 write + compare
  and `gate.sh --nodes 6` (1e9) in the new mode, ABBA of 3.8646e11 digits, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1`, 4 rounds
  (A B, B A, A B, B A), A = `comm_ofi` (default), B = `COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`; one anchor run of SHMEM today
  (`COMM_OFI=0 SHMEM_OFI_NUM_NICS=4 COMM_SHMEM_ROUND_MB=256`). Paired B − A, 95 % CI with the t distribution, n = pairs.

## Results

**`t_comm --bw`, GB/s per node, 256 MiB slabs (measured, 2 runs each):**

| nodes | comm_ofi | SHMEM today | PE per APU |
|---|---|---|---|
| 2 | 21.9 / 21.6 | 13.4 / 15.1 | 15.7 / 15.6 |
| 6 | 30.8 / 39.7 | 8.4 / 8.5 | 28.8 / 29.8 |

At 4 to 64 MiB, 6 nodes: PE per APU 30.0-32.5, comm_ofi 36.8-41.2, SHMEM today 10.2-12.4. The proxies' own put rate is 20.5-22.6 GB/s
per NIC while busy (measured). The B2 gate (≥ 25 GB/s per node at 6 nodes) passes.

**Digits and gates (measured):** gates 1 and 2 nodes, defaults and new mode: 4/4 PASS, identical; 2-node 1e10 written and
compared: identical; 6-node 1e10 written and compared in the new mode: identical (54.8 s); `gate.sh --nodes 6` in the new mode:
PASS, identical (ecalc 21.2 s). `/dev/shm` proxy files left per node after every item and run: 0.

**6-node ABBA, 3.8646e11 digits (measured, total s):**

| round | A comm_ofi | B PE per APU | B − A |
|---|---|---|---|
| 1 | 272.19 | 642.24 | +370.1 |
| 2 | 206.51 | 544.10 | +337.6 |
| 3 | 213.99 | 561.72 | +347.7 |
| 4 | 203.28 | 878.75 | +675.5 |
| mean | 224.0 (sd 32.4) | 656.7 (sd 154.1) | **+432.7, 95 % CI [+174.3, +691.1], n = 4 pairs** |

Paired B − A: bs +146.7 s [+72.5, +220.9]; dm +251.7 s [+66.7, +436.6]; pe 0 exchange wait +999.5 s [+57.4, +1941.6]. All-to-all rate
at pe 0 per APU thread: A 6.5-8.4 GB/s, B 1.07-2.46 GB/s. Anchor, SHMEM today, one run: 719.5 s, 1.34 GB/s (an earlier N7 value:
698.5 s mean of 2). All runs VERIFY OK on all 6 nodes.

## Verdict
Correct (every digit check identical) and the bandwidth gate passes in `t_comm`, but **do not adopt**: the full run at 6 nodes is
2.9 × slower than `comm_ofi` (CI excludes 0) and about equal to SHMEM today (one anchor run, 9 % under it; no claim). The t_comm gain
(28.8-29.8 GB/s per node, 3.4 × SHMEM today) does not carry into ecalc, where the exchange rate is 1.1-2.5 GB/s per APU thread against
about 7.2 in `t_comm` at 29 GB/s per node. The parameter stays 0.

## Open issues
- Why the ecalc exchanges run far below the `t_comm` rate in the new mode is not diagnosed (hypotheses only: serialization of the four
  APU threads behind the proxies' command ring and quiet, many smaller exchanges, `/dev/shm` pages in the copy path; init is also 32-68 s
  against 31-43 s).
- Run-to-run spread of the new mode is large (sd 154 s; round 4 878.8 s, no error).
- Value 2 (two NICs per APU, the target) is not built; aac7 has one NIC per APU, so it cannot be measured here.
- `mem_model.py` is not extended for the per-PE pools (the C rule sizes the pool).

## RESUME
Branch `n7b-b3` (docs/results only); code merged earlier as 8acda73. Next, if wanted: per-phase diagnosis of the exchange rate in the new mode at 2
nodes from COMM_XSTATS lines, before any further 6-node time.
