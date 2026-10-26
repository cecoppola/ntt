# OFI_FABRIC — comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate

Condensed 2026-10-19 (task 8b) from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`, raw data and
drivers in `archive:results_raw_2026-10-19/`. Old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: **m** measured, **d** derived
from measured numbers, **mod** modelled, **a** assumed. Times Eastern.

## Topic summary

- **Superseded in part (2026-10-25, Q103).** `comm_ofi.c` (`COMM_OFI`, `COMM_OFI_POOL`, `COMM_XSTATS`) was removed; the libfabric data plane is now `xshmem_ofi.c` (path 1, `README.md`). The fabric measurements below stay valid; the switch names in them refer to the removed transport, and the bullets "Why comm_ofi exists", "Adopted and current" and "Memory" describe it as it was (`COMM_OFI` no longer exists; `COMM_OFI_POOL_MB` and `COMM_OFI_NICS` remain, README).
- **Why comm_ofi exists.** Cray OpenSHMEMX binds one NIC per PE, and ecalc runs one process per node with four APU threads, so
  all of a node's exchange data went through one 200 Gb/s NIC (23.3 GB/s raw, m). One process can drive all four cxi NICs through
  libfabric (93.3 GB/s per node, m, §NIC16_experiments). `comm_ofi` does that for the data of every device exchange of
  `comm_shmem.c`; SHMEM keeps init, control words, signals and barriers.
- **Adopted and current.** `COMM_OFI` defaults to 1 wherever a cxi NIC is present (user decision 2026-10-06; `COMM_OFI=0` restores
  the SHMEM-only path exactly). Chunk 4 MiB and window 64 per NIC stay (tuning is flat, §TUNE17). Digits identical everywhere tested.
  Bench: all-to-all aggregate per node 44-48 GB/s at 4-10 nodes vs SHMEM 12-15 (m, §OFI17); full 10-node run 3532 s -> 1670 s
  (-52.7 %, m, §STD17), after which the part-file write to NFS (897.6 s) is the largest phase.
- **Memory.** Under comm_ofi staging lives in a per-device comm pool (`COMM_OFI_POOL_MB`); the planner, `mem_model.py` and `mnrun.sh`
  count it since OFIMEM (§STD17). B7 (the general map's v-slots, about 13 GB per node outside the layout) was confirmed there; since
  2026-10-12 the v-slots come from the comm pool by default (`COMM_LAYER_VSLOT_POOL`, §XEFF item 2), see ecalc/README.md.
- **Where the rate is still lost.** Layered (inter-stage) exchanges run at 78-82 % of the NIC at 4 nodes but 10-11 GB/s per APU thread
  at 10 nodes against 23.3 raw; direct exchanges (redistribution, results, spills) run near 1.9 GB/s and absorb skew (§XEFF, §C02).
  Of the items proposed in §XEFF: peer order (`rot`, now spelled `shift` in `COMM_SHMEM_PEER_ORDER`; wall-neutral, SHMEM_ITEMS),
  `COMM_XSTATS` (built), VSLOT_POOL + INTER2 (built; INTER2 default on until 2026-10-21, now 0, results/SHMEM.md); item 4 (OFI-native signalling) was shown NOT worth building (X3).
  §C02's `rr`/`winK` schedules were designed and a probe queued; the engine switch was not built.
- **Four PEs per node** (§P16) wins at 4 nodes / 10^10 (26 s vs 35-45 s, m) but loses 3.7x at 8 nodes, 2.4x at 4 x 10^10, does not
  fit memory at the target and needs per-PE plumbing; superseded by the proxy-PE work (`COMM_SHMEM_PE_PER_APU`, `archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`,
  results/SHMEM.md). The one-PE-per-APU rewrite was not built.
- **uffd.** `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, results/CRASHES_HANGS.md §S46); the 46-run soak (§T12)
  found 0 crashes, which is consistent with but does not prove removal.
- **Superseded/dated:** NIC16's own 2-node confirmation (its probes failed on a gres option; the `t_comm --bw` drivers of §OFI17
  replace it); §TUNE17 two-NICs-per-device has no gain on aac7 (one NIC per APU) and is correctness-only there; §P16's open items 1-9
  are history except the `MN_SELFTEST_GROW` finding (a launch of 32 or more node-processes stops at the layered self-test unless the
  switch is set; see ecalc/README.md for its current default).


The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/OFI_FABRIC.md` (2026-10-25); section anchors that cited them now point to this summary.
