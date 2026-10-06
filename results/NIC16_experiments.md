# NIC16 — one process driving all Slingshot NICs of a node (aac7)

Agent report, not committed. Times Eastern. Hardware: aac7 node x9000c1s6b1n0 (job 12294), 4 × Cassini "SS11 200Gb 2P"
(cxi0..cxi3 = hsn0..3, NUMA 0..3, one per MI300A; cxi0+cxi1 share one card serial, cxi2+cxi3 another), link BS_200G,
MTU 2112, libfabric 2.3.1 (`/opt/cray/libfabric/2.3.1`), no Slurm VNI env (default CXI service, svc_id 1).

## RESUME (updated 2026-10-05 22:55 EDT)

- **Answer found (measured):** ONE process opening one libfabric `cxi` fabric/domain/EP/CQ/AV per device (`domain_attr->name`
  = "cxi0".."cxi3") drives all 4 NICs at 23.33 GB/s each = **93.3 GB/s per node** (1 MiB `fi_write`, window 64),
  from host memory or `hipMalloc` memory (93.2), with 4 threads **or a single thread** (93.3). 4.0× one NIC.
- Coexists with Cray OpenSHMEMX in the same process (1 PE + 4 extra domains: 93.3 GB/s; SHMEM still works).
- Cray SHMEM and Cray MPICH: one PE / rank = exactly one NIC (man pages; MPICH striping CVARs measured inert).
- Tools/scripts on aac7 `~/nic16/`: `nicbw.c` (copy: `ecalc/tests/t_nicbw.c`), `build.sh`, `run1.sh`, `ctr.sh`,
  `cdiff.py`, `cxw.sh`, `mpibw.c`, `mrun.sh`. Binaries: `nicbw` (host), `nicbw_g` (HIP, ROCm 7.0.3), `nicbw_s` (+SHMEM).
- Also: remote MR counters work (93.2 GB/s); VMM (`hipMemCreate`) buffers cannot be registered (EFAULT) — register the
  comm pool (hipMalloc / host) instead; 8 domains (2 per NIC) in one process work (90.4).
- **2-node confirmation: running unattended on aac7 (started 2026-10-05 ~23:10 EDT).** `~/nic16/mn.sh` (nohup, PID 1970543)
  waits for `~/p16/R16/RUN16_DONE`, then runs inside job 12287 on its first 2 nodes (`srun --overlap -N2 -n2
  --ntasks-per-node=1 -c 192 --gres=gpu:24`, NIC i → peer NIC i, counters on both nodes via `run2.sh`):
  nicbw 1 NIC uni; 4 NICs uni / bidir; 1-thread uni / bidir; hipMalloc uni; hipMalloc + remote counters; 1 MiB and
  64 MiB writes; hybrid nicbw_s (SHMEM PE + 4 domains); 8 domains; then Cray SHMEM `shbw` 1 PE/node and 4 PEs/node
  (NUMA). Each test has `timeout 120`. `~/nic16/fin.sh` (nohup, PID 1971765) copies the log to
  `~/nic16/2N_results.txt` and writes `~/nic16/2N_DONE` when mn.sh ends.
- **Collect:**
  ```
  R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
  $R 'ls ~/nic16/2N_DONE && grep -E "^===|total|TOTAL|counters|cxi[0-3] tx|rror" ~/nic16/2N_results.txt | head -150'
  $R 'pgrep -af "nic16/(mn|fin).sh"'     # still waiting / running?  kill only by these PIDs
  ```
  Pass = 4 NICs uni ≈ 4 × the 1-NIC uni number (≈ 90+ GB/s per node), all 4 NICs' tx counters on the sender node.

## Counters

`~/nic16/ctr.sh` reads `/sys/class/cxi/cxiN/device/telemetry/hni_sts_{tx,rx}_ok_octets` before/after each run; the
counter GB/s is diluted by setup time (wall includes process start), the program's GB/s is the timed window.

## Results (all measured, single node, traffic NIC → switch → NIC)

| # | Method | Config | Per NIC GB/s | Aggregate | Counters |
|---|---|---|---|---|---|
| 1 | `cxi_write_bw` 1 pair | cxi0→cxi1, 1 MiB, list 256, 5 s | 24.25 | 24.25 | only cxi0 tx / cxi1 rx |
| 2 | `cxi_write_bw` 2 pairs | cxi0→1, cxi2→3, 1 MiB | 24.25, 24.23 | 48.5 | as expected |
| 3 | `cxi_write_bw` 4-ring | 0→1→2→3→0, 4 processes, 1 MiB | 23.37/21.89/23.36/21.89 | 90.5 | all 4 tx+rx ≈ 19–20 (diluted) |
| 4 | **nicbw 1 process, 4 threads** | ring 0→1→2→3→0, 1 MiB, win 64, host | 23.33 ×4 | **93.32** | all 4: 122 GB tx / 122 GB rx |
| 5 | nicbw 1 process 4 thr | 64 KiB win 256 / 16 MiB win 16 | 23.34 ×4 | 93.35 / 93.34 | |
| 6 | **nicbw 1 process, hipMalloc** (`FI_HMEM_ROCR`) | 1 MiB win 64 | 23.33/23.33/23.20/23.34 | **93.20** | all 4 |
| 7 | **nicbw 1 process, 1 thread** (round-robin over 4 EPs/CQs) | 1 MiB win 64, host | 23.33 ×4 | **93.33** | |
| 8 | nicbw 1 thread, hipMalloc | 1 MiB | 23.34/23.33/23.17/23.34 | 93.18 | |
| 9 | nicbw 1 thread | 64 KiB win 256 | 23.34 ×4 | 93.34 | |
| 10 | nicbw 1 thread | 8 KiB win 512 | 3.95 ×4 | 15.78 (op-rate bound, 1.9 Mops/s) | |
| 11 | nicbw 4 threads | 8 KiB win 512 | 3.39/6.12/6.23/6.22 | 21.95 | |
| 12 | nicbw 1 NIC (cxi0→cxi0 loopback) | 1 MiB | 23.34 | 23.34 | cxi0 only |
| 13 | nicbw **2 processes** × 4 NICs (both share every NIC) | 1 MiB / 4 MiB GPU | 11.7/10.9 per proc-NIC | 45.3 + 45.3 = **90.6** | each NIC 22–23 tx and rx |
| 14 | **hybrid**: `nicbw_s` = 1 Cray SHMEM PE + 4 libfabric domains, same process | 1 MiB win 64 | 23.33 ×4 | **93.34** | all 4; SHMEM putmem afterwards OK (22.4 GB/s to self, via cxi0) |
| 17 | nicbw, hipMalloc + **remote MR counters** (`FI_RMA_EVENT`, `fi_mr_bind(mr, cntr, FI_REMOTE_WRITE)`) | 4 MiB win 16 | 23.31 ×4 | 93.24 | target counters = ops sent (27806 / 27806 …) — completion signalling works at full rate |
| 18 | nicbw, **VMM** buffers (`hipMemCreate`+`hipMemMap`+`hipMemSetAccess`, as ecalc's dbig arena) | — | — | **fails** | `fi_mr_regattr` = −14 EFAULT, "cxil_map: write error"; same with `FI_HMEM_ROCR_USE_DMABUF=0`, with `FI_HMEM_SYSTEM`, with `FI_CXI_ODP=1` |
| 19 | nicbw, `hipHostMalloc` buffers (flags 0 / NumaUser) | 4 MiB win 16 | 23.25/21.9/21.9/21.9 | 89.0 / 88.8 | buffers not NIC-local (cxi0's NUMA) cost ≈ 6 % on NICs 1–3 |
| 20 | nicbw **8 domains** in one process (2 per NIC; target-like) 4 thr / 1 thr | 4 MiB win 16 | 11.7/10.9 per domain | 90.44 / 90.43 | 2 flows per NIC |
| 21 | **Cray SHMEM 4 PEs/node** `shbw` (putmem_nbi+quiet, ring PE→PE+1, `SHMEM_OFI_NIC_POLICY=NUMA`, `-n4 -c 48`) | 4 MiB W16 / 64 MiB W4 / 256 MiB W2 / 1 MiB W64 | 23.24–23.26 | 93.00 / 93.27 / 93.30 / 93.02 | all 4 NICs (intra-node SHMEM goes over the NIC) |
| 22 | nicbw 1 process 4 NICs, large writes | 256 MiB W2 / 64 MiB W4 | 23.33 ×4 | 93.34 / 93.34 | no large-message slump on one node |
| 23 | hybrid `nicbw_s`, buffers = slices of the **Cray SHMEM symmetric heap** (`shmem_malloc`, host hugepages; `NICBW_SHHEAP=1 SHMEM_SYMMETRIC_SIZE=2G`) registered again in the 4 extra domains | 4 MiB W16 | 21.36/21.52/23.34/21.36 | 87.58 | works; heap sits on one NUMA node (cxi2's), the other 3 NICs lose ≈ 8 %. SHMEM putmem afterwards 23.1 GB/s |
| 15 | Cray MPICH 9.1 `mpibw`, 2 ranks 1 node, `MPIR_CVAR_NOLOCAL=1` | 4 MiB, win 16 | — | 23.28 | cxi0 only |
| 16 | same + `MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_STRIPING=1 MPIR_CVAR_CH4_OFI_MAX_NICS=4` (+HASHING, threshold 64 KiB; also `MPICH_CH4_OFI_*` names) | 4 MiB | — | 23.28 | **cxi0 only** — inert |

Commands (on aac7, `~/nic16`):
- #1–3: `srun --jobid=12294 --overlap -N1 -n1 bash -lc "D=6 ~/nic16/cxw.sh cxi1:cxi0:49211 cxi2:cxi1:49212 …"`
  (server `cxi_write_bw -d <dst> -p <port>`, client `numactl -N n -m n cxi_write_bw -d <src> -p <port> -s 1048576 -D 6 localhost`).
- #4–12: `srun --jobid=12294 --overlap --gres=gpu:24 -N1 -n1 -c 192 bash -c "~/nic16/run1.sh ~/nic16/nicbw[_g] [-g] [-1] -n 0,1,2,3 -s 1048576 -w 64 -t 5"`.
- #13: `NP=2 ~/nic16/run1.sh ~/nic16/nicbw -n 0,1,2,3 -x 1 …` (two processes on the node, rank r NIC i → rank r+1 NIC i+1).
- #14: `srun --jobid=12294 --overlap -n1 -c 192 ~/nic16/nicbw_s -n 0,1,2,3 -s 1048576 -w 64 -t 5 -d x -T h1`.
- #15–16: `~/nic16/mrun.sh 2 MPIR_CVAR_NOLOCAL=1 [CVARs]`.

## The method (exact API)

Per NIC i (device name `cxi<i>`), in one process:
```
hints: fabric_attr->prov_name="cxi"; domain_attr->name="cxi<i>"; ep_attr->type=FI_EP_RDM;
       caps=FI_RMA|FI_MSG(|FI_HMEM); mr_mode=FI_MR_LOCAL|FI_MR_VIRT_ADDR|FI_MR_ALLOCATED|FI_MR_PROV_KEY|FI_MR_ENDPOINT(|FI_MR_HMEM);
       threading=FI_THREAD_DOMAIN; tx_attr->size=4096
fi_getinfo(FI_VERSION(1,20)) → fi_fabric → fi_domain → fi_cq_open(FI_CQ_FORMAT_CONTEXT, 8192) → fi_av_open(FI_AV_TABLE)
→ fi_endpoint → fi_ep_bind(cq, FI_TRANSMIT|FI_RECV), fi_ep_bind(av) → fi_enable → fi_getname
MR: fi_mr_reg (host) or fi_mr_regattr{iface=FI_HMEM_ROCR, device.reserved=gpu} (hipMalloc); FI_MR_ENDPOINT ⇒ fi_mr_bind(mr, ep)+fi_mr_enable
exchange (addr, fi_mr_key, buffer VA) out of band; fi_av_insert(peer addr of the matching NIC)
loop: fi_write(ep, buf, len, fi_mr_desc(mr), peer, peer_va+off, peer_key) up to window W; fi_cq_read to retire
```
No env needed on aac7 (default CXI service). Thread pinned to the NIC's NUMA node (`/sys/class/cxi/cxiN/device/numa_node`).

## Caveats

- `hipMalloc` in a step needs `srun --gres=gpu:24` (job shows gres/gpu=24); without it /dev/dri is empty. The ROCm 7.2.4
  HIP runtime reported "no ROCm-capable device" in the step where 7.0.3 worked — nicbw_g is linked to 7.0.3.
- Small messages: a single thread is op-rate bound (≈ 1.9 M writes/s total at 8 KiB); ≥ 64 KiB saturates all 4 NICs.
- SHMEM `SHMEM_OFI_NIC_POLICY=NUMA` aborts when the single PE spans all NUMA nodes ("numa node -1"); use default BLOCK.
