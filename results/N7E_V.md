# N7E-V — COMM_SHMEM_PE_PER_APU=2 (two helper PEs per APU): correctness record

Scope: the correctness gates of value 2 on aac7 (item n7v, 2026-10-14, commit 97275a2, 1 and 2 nodes, 1e9 and 1e10 digits). Dated
record; the code and the central docs win where they differ. Labels: m = measured.

**Question.** Does `COMM_SHMEM_PE_PER_APU=2` (1 + 8 PEs per node; an APU's staging and slabs interleaved in 4 MiB blocks over its two
helper PEs' heaps, `ecalc/comm_shmem.c` `px_map2`) give identical digits? (aac7 has one cxi NIC per APU, so both helpers share it: no
speed claim is made here.)

**Failure found, fixed.** The first 2-node gate (commit b989a6b) stopped with `the heap of helper PE N is not reachable ... too small
or unaligned`. Cause (m): `shmem_malloc` put the pool at offset 1920 mod 4096 in the helper's heap file, and `px_map2` maps the heap
files page by page (the offset is not a page multiple). Fix: with value 2 the pool is `shmem_align`ed to 2 MiB (every PE alike).
Value 1 (`shmem_ptr` or one whole-file mapping) is unchanged.

**Results (m).**

| run | result |
|---|---|
| tools/gate.sh --nodes 1, value 2 | PASS, VERIFY OK, digits identical, 1e9 |
| tools/gate.sh --nodes 2, value 2 | PASS, VERIFY OK, digits identical, 1e9; 9 PEs per node in the launch log |
| 1e10 at 2 nodes, written and compared, value 2 | identical, VERIFY OK, ecalc total 47.8 s (correctness context only) |
| `/dev/shm/ecalc_px_*` files left after each run | 0 on both nodes |

Default stays value 1; `COMM_SHMEM_PX_SEND=2` is refused with value 2.
