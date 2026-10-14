# N7C — SHMEM proxy mode: why it is slow, and `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` (config C)

Scope: the N7C record (2026-10-14, aac7, 2 and 6 nodes): diagnosis of the proxy mode (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`, N7b), the switch
`COMM_SHMEM_PX_NUMA` (default 0), gates and the 6-node ABBA against `comm_ofi`. Dated record; the code and the central docs win where they differ.

**Diagnosis (measured at 6 n in the B3 logs and at 2 n in a 12-run item; modelled where marked).** `t_comm` is host-only and sees only the NICs.
ecalc's exchanges are mostly GPU work on the pool (xGMI pushes, transposes, scatter, `hipMemcpyAsync`). Under `comm_ofi` the pool is
fine-grained device memory on the APU's HBM; in the proxy mode it is the proxy's `/dev/shm` heap: 4 KiB pages (no huge-page route on aac7),
placed by first touch (local to its APU in 6 of 24 pool-runs, m), and copies that touch it go to the SDMA engine (21 GB/s, results/I.md).
Two fixes: F1 `COMM_SHMEM_PX_NUMA=1` (mbind of the proxy heap to the NIC's NUMA node: 32 of 32 pools local, m) and F2 `HSA_ENABLE_SDMA=0` (environment
only; copies by blit kernels). At 2 nodes, 5e10 digits (m, one run each, mirrored order): comm_ofi fabric stage 8.8 s, rounds kinds 7.5 s,
total 55.1-59.3 s; proxy mode 14.7-21.8 s, 12.5-12.7 s, 80.1-99.3 s; with both fixes 9.7 s, 7.6 s, 57.1 s (the first run 136.0 s: slow init, 80 s).
Either fix alone was not enough (NUMA only: fabric 13.5-17.2 s; SDMA off only: worse, 19.7-26.0 s). Remaining causes: init +28-80 s and its skew,
4 KiB pages (admin change), proxy batch-then-quiet coupling (docs: design only).

**Change.** `COMM_SHMEM_PX_NUMA` (0 = off = default; 1 = `mbind` the proxy heaps to the NIC's NUMA node before registration; pe 0 reports
pool page placement and page size). `ecalc/comm_shmem.c`, README and registry rows. `HSA_ENABLE_SDMA=0` is an environment setting and is added to no launch line.

**Test (measured, one queue item, 6 nodes, build of branch n7c; node screen: nothing excluded).** Config C = `COMM_OFI=0
COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 HSA_ENABLE_SDMA=0`.
- Gates `tools/gate.sh` (1e9): 1 and 2 nodes with config C: PASS, digits identical; 1 and 2 nodes at the defaults: PASS, identical.
- 1e10 write + compare with config C: 2 nodes identical (44.2 s), 6 nodes identical (39.4 s). All runs VERIFY OK; `/dev/shm` proxy files left per node: 0.
- 6-node ABBA, 3.8646e11 digits, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1`, 4 rounds (A B, B A, A B, B A); A = `comm_ofi` default, B = config C:

| round | A comm_ofi s | B config C s | B − A s |
|---|---|---|---|
| 1 | 235.05 | 385.08 | +150.0 |
| 2 | 210.39 | 460.17 | +249.8 |
| 3 | 219.02 | 461.66 | +242.6 |
| 4 | 200.91 | 527.96 | +327.1 |
| mean | 216.3 (sd 14.5) | 458.7 (sd 58.4) | **+242.4, 95 % CI [+127.1, +357.7], n = 4 pairs** |

Paired B − A: bs +90.5 s [+20.3, +160.8]; dm +129.3 s [+33.6, +225.0]; pe 0 exchange wait +425.2 s [+30.9, +819.4]. Pe 0 all-to-all rate per
APU thread: A 6.8-8.3 GB/s, B 2.0-4.0 GB/s (per-round values in the item summary).
- One B run with `HSA_ENABLE_SDMA` at its default (`PX_NUMA=1` only): 417.7 s (single run, m; inside B's spread, no claim on the copy share).
- Before (N7b B3, results/N7B.md, other build): proxy mode 656.7 s vs comm_ofi 224.0 s, +432.7 s [+174.3, +691.1].

## Verdict
Correct (every digit check identical, gates PASS), switch merged off by default. **Not adopted:** at 6 nodes config C is still 2.1 x slower
than `comm_ofi` (+242.4 s, CI excludes 0), against 2.9 x before; the 2-node parity (57.1 vs 55.1 s) does not carry to 6 nodes with 5 peers and layv exchanges.
Placement and SDMA explain roughly 45 % of the gap (modelled from the means, 432.7 to 242.4 s; builds and days differ); the rest is open.

## Open issues
- 6-node gap of +242 s: the 5-peer lock-step with proxy batch-then-quiet (F4: per-peer contexts or signal put with the data) and the init/skew
  (B init 35.8-74.5 s vs A 31.8-35.4 s) are untested hypotheses. The spread of B is large (sd 58 s).
- Targeted copy kernel instead of the global `HSA_ENABLE_SDMA=0` (F2b) is not built.
- 4 KiB pages need an admin change (`shmem_enabled`, `huge=` on `/dev/shm`) or reserved huge pages.

## RESUME
Merged to main with the switch off. Next, if wanted: F4 design, per-phase init timestamps at 2 nodes, a 6-node run with `COMM_LAYER_STATS=1`
in config C to see which exchange kinds (layv) still lag.
