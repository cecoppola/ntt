# OFI17 — `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`)

Agent report. Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.
Design: docs/code/07_COMM_OFI.md. Method: results/NIC16_experiments.md.

## RESUME (updated 2026-10-06 01:05 EDT)

- **Built** on branch `p17-ofi` (not merged): `ecalc/comm_ofi.c`, `ecalc/comm_ofi.h`, hooks in `ecalc/comm_shmem.c`, Makefile
  (`OFI=1` automatic with `/opt/cray/libfabric/2.3.1`), README rows, `ecalc/tests/ofi17_drive.sh`. Switch `COMM_OFI=1`, off by default.
- aac7 clone: `~/ofi17` (pull from GitHub, `make -s -j48 SHMEM_CRAY=1` after `source ecalc/aac7env.sh`; tools built too).
- **Unit (1 node, job 12294) passed** — see §2.
- **Armed** (fire-and-forget): `~/ofi17/ecalc/tests/ofi17_drive.sh 12287` (nohup, PID 2037988 on the aac7 login node) waits for
  `~/p16/R16/RUN16_DONE` AND `~/nic16/2N_DONE` (deadline 08:56 EDT), then for job 12287's other steps to end, then runs:
  tc2 (t_comm at 2 nodes, OFI 1/0) → acc (mnaccept unit,e9,mn, `COMM_OFI=1 MNRUN_NODES=2`) → bw2 (t_comm --bw 2 nodes, both) →
  ab (1e10 per node at 2 and 4 nodes, OFI 0/1 interleaved, 2 each, `MN_COMM_MARK=1`) → bwn (t_comm --bw at 4, 8, 10 nodes, both).
  Writes `~/ofi17/R/drive/DONE` (first line = verdict), `drive.log`, `walls.txt`, logs.
- Also queued: `~/ofi17/acc1.sh` (1-node `mnaccept --only mn` with `COMM_OFI=1` on 12294 once the other agent's mnaccept there,
  PID 2027981, exits) → `~/ofi17/R/acc1b_summary.txt`, `~/ofi17/R/ACC1_DONE`.
- **Collect**:
  ```
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
  $R 'head -1 ~/ofi17/R/drive/DONE; cat ~/ofi17/R/drive/drive.log | cut -c1-300 | tail -80; cat ~/ofi17/R/drive/walls.txt'
  $R 'cat ~/ofi17/R/acc1b_summary.txt'
  $R 'ps -eo pid,etime,args | grep -E "[o]fi17_drive|[a]cc1.sh"'     # kill only by these PIDs
  ```

## 1. Design (summary; full: docs/code/07_COMM_OFI.md)

- One process per node, the in-process xGMI stage, Cray SHMEM for init / control words / barriers / host collectives: unchanged.
- Under `COMM_OFI=1` the *data* of every device exchange of comm_shmem.c (alltoall, alltoallv, allgather, the rounds) goes as
  `fi_writemsg(FI_DELIVERY_COMPLETE)` chunks (4 MiB, window 64 per NIC) over the NICs of the calling APU thread's device (one
  libfabric `cxi` fabric/domain/EP/CQ/AV per NIC, a per-thread list: `COMM_OFI_NICS="0,4;1,5;…"` for 2 NICs per APU). Once all
  chunks to a peer completed, the existing SHMEM signal word is put; the receiver's protocol is untouched.
- Registered memory: a comm pool per device (`COMM_OFI_POOL`: fine-grained device memory by default, hipMalloc, or NUMA-local
  host), sized `COMM_SHMEM_POOL_MB/4 + 256` MiB; staging and `comm_sym_alloc` of OFI communicators come from it.
- Integration: a data plane inside comm_shmem.c (a per-exchange base pointer `db`, ~60 changed lines) + comm_ofi.c (≈ 330 lines).
  Not a new comm_ops: it reuses the sequences, roff publication, rounds, counts check and staging logic.
- Endpoints: 4 × k per process, independent of the number of communicators.

## 2. Unit tests (1 node, job 12294, Cray OpenSHMEMX + libfabric 2.3.1, all measured)

| test | command (in `~/ofi17/ecalc`, `COMM_SHMEM_POOL_MB=1024`, `SLURM_JOB_ID=12294 MNRUN_NODES=1`) | result |
|---|---|---|
| t_comm 2 PEs, OFI | `mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 COMM_OFI_VERBOSE=1 ./tests/t_comm` | VERIFY OK ×2; data on cxi0 (31–32 writes, 0.05–0.06 GB per PE) |
| t_comm 4 PEs, OFI (strided sets → devices 1, 2 → cxi1, cxi2) | same with 4 | VERIFY OK ×4; cxi0, cxi1, cxi2 carry the writes |
| rounds | + `COMM_SHMEM_ROUND_MB=1 COMM_OFI_CHUNK_MB=0.25` | VERIFY OK ×4; 42 exchanges through the rounds' path, 18 in 285 rounds |
| 2 NICs per device, small window | + `COMM_OFI_NICS="0,1;1,2;2,3;3,0" COMM_OFI_CHUNK_MB=1 COMM_OFI_WINDOW=4` | VERIFY OK ×4; device 1 stripes over cxi1 + cxi2; all 4 NICs carry writes |
| baseline | `COMM_OFI=0`, 4 PEs | VERIFY OK ×4 |
| ecalc 1e8 at 2 node-processes, OFI (HIP build: fine-grained device pools registered with `FI_HMEM_ROCR`, ROCm 7.2.4) | `mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 POOL_LOG=27 ./ecalc 100000000 <f>` | VERIFY OK both; `digcmp.sh` **identical** to ref/e_100000000.txt; each device 0.42–0.43 GB in ≈ 760 writes on its own cxi |

Bug found and fixed on the way: the cxi provider returns `mr_mode` without `FI_MR_VIRT_ADDR` (offset-based RMA addresses); the
first run died with `ADDR_OUT_OF_RANGE`. The blob now carries base 0 in that case (commit cdf1043).

Traps met: (1) another agent (`~/fix17a`) runs `mnaccept` in job 12294 at the same time and both default to
`MNACCEPT_TMP=~/p16/mnaccept_tmp` (`rm -f $TMP/*` at start) — use a private `MNACCEPT_TMP`; (2) a fresh clone needs `make -C
../tools` (digcmp.sh uses `../tools/unpack_digits`; without it every comparison prints DIFFERS).

Per the coordinator's constraint, no bandwidth sweep ran before `RUN16_DONE` (the 10-node production run in 12287).

## 3. Multi-node (job 12287) — pending (the driver)

## 4. Open items

- The SHMEM pool keeps its full size under `COMM_OFI=1` while the device staging moves to the comm pool: the node holds both
  (≈ the SHMEM pool again per node). Shrinking it needs the pool rule (`binsplit_shmem_pool_rule`, mem_model) to know about OFI.
- The budget check / mem_model do not count the comm pool.
- Two NICs per APU are exercised only as two NICs of different NUMA nodes per device on aac7 (one NIC per APU there).
