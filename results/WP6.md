# WP6 — inter-node correctness on aac6 (2026-09-17/18)

Branch `wp6-internode` (from `wp1-decimal-base` at 8a43393). Code under
test, unchanged from that base: `ecalc/comm_tcp.c` (TCP full mesh, one
process per rank), `ecalc/ntt_dist.c` (the distributed four-step
transform), `ecalc/tests/t_dist.c` (one process per rank over TCP when
`COMM_RANK` is set, rank r on APU r mod 4), `ecalc/wp6run.sh` (one process
per APU across the nodes of a Slurm allocation, `COMM_HOSTS` from
`scontrol show hostnames`, port base 27000). One addition on this branch:
`ecalc/tests/t_comm.c` also runs as one process per rank when `COMM_RANK`
is set, so the communicator alone can be run across nodes with
`wp6run.sh <nodes> ./tests/t_comm` (all-to-all of 1 B … 3 MiB slabs
checked word-for-word against the senders, barrier, max and mod-q
reductions). Correctness only: aac6 has one 1 GbE NIC per node.

Build on the login node (`module load rocm; make -s tests/t_dist
tests/t_comm`) in `~/ntt-wp6` (a clone of `~/ntt` with this branch fetched
from a bundle). All runs were launched by a nohup watcher on the login node
the moment a job named `wp6` was RUNNING, so that a short window could not
be missed; logs `~/wp6run-<jobid>.log`, `~/wp6extra-20656.log`, snapshots
in `~/wp6status.log`.

## Cluster state

Partition `PPAC_MI300A_SPX`, 4 usable-in-principle nodes: s24-30 and s24-35
**down** for the whole session (so 3 nodes / 12 ranks was impossible);
s24-16 held by the main session's `wp1d` jobs (20644 until 02:59 aac6
time, then 20655 for 4 h); s24-26 held by `admin`'s `cdash-nightly`
(20645_2, 3 h) until ≈ 00:55, then "planned" for the pending 2-node `wp6`
request 20649. Request 20649 (`-N2 --gpus-per-node=4 -t 1:00:00 -J wp6`,
submitted by the main session) started at 02:59:31 when 20644 ended and
was cancelled 22 s later (from the main session, to let 20655 in), which
was enough for two of the three planned runs. A second 2-node request
(20657, 2 h) was queued afterwards; it can only start when 20655 ends.

## Runs

| job | nodes | ranks | test | result |
|---|---|---|---|---|
| 20649 | s24-16 + s24-26 | 8 (4 per node, one per APU) | `wp6run.sh 2 ./tests/t_comm` | **VERIFY OK on all 8 ranks** (5 slab sizes, barrier, reductions) — 7 s |
| 20649 | s24-16 + s24-26 | 8 | `wp6run.sh 2 … ./tests/t_dist 20` | **VERIFY OK on all 8 ranks** (4 checks each: 2¹⁰ × 2¹⁰, all four primes) — 3 s |
| 20649 | s24-16 + s24-26 | 8 | `t_dist 24` | not run: job cancelled before it started (`srun: error: Slurm job 20649 has expired`) |
| 20656 | s24-26 | 4 (TCP, one process per APU) | `wp6run.sh 1 ./tests/t_comm` | VERIFY OK on all 4 ranks |
| 20656 | s24-26 | 4 | `t_dist 20`, `t_dist 24`, `t_dist 26` | VERIFY OK on all 4 ranks (4, 52, 65 checks; up to 2²⁶ points) |
| 20656 | s24-26 | 4 | `t_dist 20` again immediately after, same ports | VERIFY OK (port reuse after a clean exit is fine) |
| 20656 | s24-26 | 8 (two processes per APU, `srun --ntasks-per-node=8`, `COMM_PORT=27100`) | `t_dist 24` | **VERIFY OK on all 8 ranks** (52 checks each) |
| login node | aac6-fe1 | 4 and 8 forked | `t_comm` (host-only build) | VERIFY OK |
| 20657 | (2 nodes, pending) | 8 | `t_comm`, `t_dist 20`, `t_dist 24` | see the addendum below if it ran |

So: the communicator and the distributed transform are correct **across
two nodes with 8 ranks** (t_comm and t_dist 20), the transform is correct
with **size 8 up to 2²⁴ points** (8 ranks on one node — the ntt_dist code
does not know where a rank lives, only the socket path differs) and with
size 4 up to 2²⁶ points over TCP. Every rank verifies the full
convolution result of its own block-cyclic rows against the one-rank
engine (`ntt_fwd/ntt_pw/ntt_inv`) with the same random input.

## Bugs found

None. No source change was needed in `comm_tcp.c`, `t_dist.c` or
`wp6run.sh`; `t_comm.c` gained the per-rank mode (commit fa3e603).

## Observations on the TCP communicator across nodes

- **Connection ordering.** Rank r listens on `base + r` (INADDR_ANY), then
  connects upward to every rank > r (retrying for up to 60 s while the
  peer's process starts), then accepts from every rank < r and reads the
  8-byte hello that identifies the connecting rank. Because every rank
  listens before it connects and `connect` only needs the peer to be
  listening (backlog 64), the order in which srun starts the 8 processes
  on the two nodes does not matter; the start-up skew across nodes was
  well under a second (all META lines carry the same second). With 4 ranks
  per node the ports are `27000+r`, distinct per rank, so two nodes never
  clash; two processes per APU (8 ranks on one node) work the same way.
- **Port reuse.** The listening socket has `SO_REUSEADDR` and is closed
  after the accepts; consecutive runs 0–5 s apart on the same ports
  (t_comm → t_dist 20 → t_dist 24 → 26 → 20) all connected at once
  — TIME_WAIT sockets from the previous run did not block the bind.
- **Host names.** `scontrol show hostnames` gives `ppac-pl1-s24-NN`, which
  resolves (via `/etc/hosts`) to the node's 10.194.42.x address on the
  1 GbE interface `enp129s0` — no IP list is needed in `COMM_HOSTS`.
- **No deadlock in the all-to-all.** One sender thread per peer writes
  the slab while the caller's thread reads from every peer in rank order;
  with 8 ranks and 3 MiB slabs (t_comm round 5, larger than the socket
  buffers) and 2²⁴-point planes in t_dist (slabs of 2 MiB per peer) the
  blocking sockets never stalled, as the design argues (every send has an
  independent reader).
- **Speed (for the record only).** `t_dist 20` on 8 ranks over two nodes:
  3 s wall including process start; the 1 GbE link is irrelevant at these
  sizes.
- **Slurm.** `srun --ntasks-per-node=4 --gpus-per-node=4` exposes all four
  APUs to every task, so `hipSetDevice(rank % 4)` is what places the
  ranks (the 8-rank step used `--overlap` and `COMM_PORT=27100`).
