# AAC7 survey (read-only) — 2026-10-03, 22:39–22:50 EDT

Login `chcoppola@aac7.amd.com` (direct; same password as aac6). Login node is `uan1` (HPE Cray EX "user access node").
All numbers below are **measured** unless marked otherwise. Two survey jobs were run (`srun -N1 -t 2`, `srun -N2 -t 2`,
≤ 1 min each); nothing was built or modified on either cluster.

## 1. Slurm

Slurm 25.11.4 (aac6: 23.11.11). Account `mpo`, cluster `midgard`, QOS `normal` (no per-user MaxNodes/MaxWall/MaxJobs
set in the association). No reservations.

| Partition | Nodes | State (22:40 EDT) | GRES | MaxTime | MaxNodes | Notes |
|---|---|---|---|---|---|---|
| `192C4G1H_MI300A_RHEL9_A1` (**default**) | 13 | 3 allocated, 10 idle | gpu:4 (S:0-3) | **5-00:00:00** | UNLIMITED | 13 × 4 MI300A = 52 APUs |
| `192C4G1H_MI300A_RHEL9` | 14 | same 13 + the CPX node | gpu:4 / gpu:24 | 5 d | UNLIMITED | superset incl. x9000c1s6b1n0 (CPX) |
| `192C4G1H_MI300A_RHEL9_A1_CPX` | 1 | idle | gpu:24 | 5 d | 1 (QOS hlrs_class, gres/gpu=1) | CPX mode, not for us |
| `192C4G1H_MI300A_RHEL9_A0` | 8 | 7 drained*, 1 inval | gpu:4 | 5 d | UNLIMITED | A0 silicon, all down |

Node list (A1): `x9000c1s0b0n0, x9000c1s0b1n0, x9000c1s1b0n0` (allocated to skoranne for 2–3 days),
`x9000c1s1b1n0, x9000c1s2b0n0, x9000c1s2b1n0, x9000c1s3b0n0, x9000c1s3b1n0, x9000c1s4b0n0, x9000c1s4b1n0,
x9000c1s5b0n0, x9000c1s5b1n0, x9000c1s6b0n0` (idle). Each: `CPUTot=192 Sockets=4 CoresPerSocket=24 ThreadsPerCore=2
RealMemory=500000 Gres=gpu:4(S:0-3) Features=MI300A,rocm,amdgpu`, OS RHEL 9.6 kernel 5.14.0-570.12.1.el9_6.

`srun -p 192C4G1H_MI300A_RHEL9_A1 -N2 -n2 -t 2 --gpus-per-node=4` started immediately (10 idle nodes).
Default `srun -N1` without `-c` confines a task to 2 CPUs (cgroup); use `-c 192`/`--exclusive` for real runs.

## 2. Node hardware

| | aac7 compute (x9000c1s1b1n0, measured in job) | aac7 login (uan1) |
|---|---|---|
| APUs | 4 × AMD Instinct MI300A (0x74a0, gfx942, `sramecc+:xnack-`) | none (amdgpu not loaded; rocm-smi errors) |
| CPU | 192 logical (4 × 24c × 2 SMT), model string "AMD Instinct MI300A Accelerator" | 128 logical, 251 GiB |
| Memory | `free -g`: 501 GiB total, 341 free, 418 available | |
| Local disk | `/tmp` = tmpfs 251 GiB; `/` overlay 251 GiB (175 GiB avail); two 3.5 TB NVMe (`nvme0n1`, `nvme1n1`) present but **not mounted** anywhere we can see (no `/ssd0`, `/scratch`) | `/` 3.0 TB, 2.9 TB free |
| ROCm on node | `/opt/rocm` → `/opt/rocm-7.0.3` | same |

Identical APU count/model and memory class to aac6 (aac6: 4 × MI300A, RealMemory 514000 MB, 192 CPUs).

## 3. Interconnect (the key finding)

aac7 is an **HPE Cray EX with Slingshot-11**. On each compute node:

```
lspci: 4 × "Cray Inc Cassini 1 [Slingshot 200Gb]"  (0000/0001/0002/0003:01:00.0)
hsn0..hsn3   Speed: 200000Mb/s  Link detected: yes      (one NIC per APU socket)
/dev/cxi0 /dev/cxi1 /dev/cxi2 /dev/cxi3 /dev/cxi_sbl ; /sys/class/cxi/cxi{0..3}
fi_info -p cxi: provider cxi, domains cxi0..cxi3, FI_EP_RDM, FI_PROTO_CXI   (libfabric 2.3.1)
hsn0 IPs: 10.150.0.47/16 (x9000c1s1b1n0), 10.150.0.51/16 (x9000c1s2b0n0)   -> TCP over HSN also possible
bond0 (enp129s0, Intel I210): 1000Mb/s, 10.168.0.x/22  (management/NFS)
```

So: **4 × 200 Gb/s Slingshot per node = 800 Gb/s injection**, vs aac6's single 1 GbE (aac6 login shows `mlx5_0`
InfiniBand HCA but the IB ports `enp67s0f*` are DOWN and the PPAC nodes talk over 1 GbE). No IB verbs on aac7
(`ibv_devinfo` not installed; Slingshot uses libfabric/cxi, not verbs). The login node has only 1 GbE (no cxi).

The aac7 MI300A nodes are a **separate Slurm cluster** (`midgard`, controller on aac7) from aac6's `PPAC_MI300A_SPX`
(cluster `prerelease`). A job cannot span both.

Cross-cluster reachability (measured): `aac6-fe1 -> aac7.amd.com:22` TCP **OPEN**, `uan1 -> aac6.amd.com:22` TCP
**OPEN**, ICMP ping blocked. Both go through public addresses (aac7 = 209.11.132.110, aac6 = 216.114.73.106), i.e.
the WAN path from the login nodes; compute nodes of either cluster have only private addresses (aac7: 10.168/10.150;
aac6: 10.194.42.x) and are not routable to each other. A combined aac6+aac7 run would have to be TCP via ssh tunnels
from login nodes over the internet — **not useful for the NTT traffic volumes** (modelled: slower than aac6's 1 GbE).

## 4. Software

- **ROCm**: `/opt/rocm-7.0.3` (`which hipcc` on login = `/opt/rocm-7.0.3/bin/hipcc`, already on PATH without any
  module). Modules: `rocm/7.0.3`, `amd/7.0.3`, `rocm-new/{7.1.1,7.2.0(default),7.2.1,10.0.0}`,
  `rocm-afar/{7.0.0,7.1.1,22.1.0,22.2.0,22.3.0}`, `rocm-therock/{23.1.0,23.2.1}`, and `/shareddata/modules/base`
  `rocm/{7.2.3,7.2.4,7.12.0,7.13.0,7.14.0,afar-*}`. aac6 default is rocm/7.2.4 (HIP 7.2.53211) — aac7's 7.2.4
  module should match if a bit-identical toolchain matters; `rocm/7.0.3` is the system default.
- **Cray PE** (`cpe/26.03`): PrgEnv-cray (loaded by default), PrgEnv-amd, PrgEnv-gnu; `cce/21.0.0`, `gcc-native/14`,
  `craype-accel-amd-gfx942`, `craype-network-ofi`.
- **MPI**: `cray-mpich/9.1.0` (default; `/opt/cray/pe/mpich/9.1.0/ofi/cray/20.0`), 8.1.32/8.1.33/9.0.x,
  cray-mpich-ucx; plus `openmpi/5.0.10-libfabric2.3.1-xpmem-2.7.4-rocm-7.0.3`, `openmpi/5.0.10-ucc1.6.0-ucx1.19.1-...`
  in `/shared/apps/modules/rhel9`.
- **OpenSHMEM**: `cray-openshmemx/11.8.0` (default) and 11.7.4. Needs `module load cray-dsmml` first (prereq;
  `PrgEnv-cray` is already loaded so do **not** load PrgEnv-amd on top — it conflicts). Files:
  `/opt/cray/pe/sma/11.8.0/ofi/sma/include/shmem.h`, `/opt/cray/pe/sma/11.8.0/ofi/sma/lib64/libsma.{a,so}`.
  No `oshcc`/`oshrun` wrappers (Cray uses `cc`/`CC` + `-lsma`; launch with `srun`). `cray-pmi/6.1.17`,
  `libfabric/2.3.1`, `cray-dsmml/0.3.1`. **No Sandia OpenSHMEM** (SOS) installed; `~/sos` is absent (different home).
- Other: `cray-python/3.12.12`, `cray-fftw`, `cray-libsci`, `perftools`, `papi`, `rccl-tests/7.0.3`.
- **Login builds**: hipcc is available on the login node (unlike aac6, no `module load rocm` needed).

### Filesystems

| Mount | aac7 | aac6 |
|---|---|---|
| `$HOME` | `/shared/midgard/home/chcoppola` (NFS `172.23.0.26:/midgard_home`, 11 T, **3.1 T free**, 71 % used) | `/shared/prerelease/home/mpo_2026/chcoppola` (NFS `10.194.42.250:/AAC_MI300`, 67 T, 26 T free) |
| Same filesystem? | **No.** `~/ntt`, `~/sos`, `~/ntt-*` do not exist on aac7; home is empty of our files. | |
| Other NFS | `/shared/apps` (11 T, 9.4 T free), `/shareddata` (5.1 T, 3.9 T free), `/pe`, `/cm_cpe` (ro) | `/shareddata`, `/nfsapps` |
| Lustre | none | none |
| Node-local | `/tmp` tmpfs 251 GiB (RAM-backed; counts against the 500 GiB); NVMe present, unmounted | per phase-7 notes |

All NFS mounts are v3 over TCP (`rsize/wsize=65536`, `hard`), same class as aac6; the 4 × 10¹⁰ reference and bundles
would have to be copied over (scp via the login nodes; both directions on port 22 are open).

## 5. Current load (22:40 EDT, Sat 2026-10-03)

```
JOBID  PARTITION              USER      STATE    TIME        NODES  NODELIST
12091  192C4G1H_MI300A_RHEL9  skoranne  RUNNING  2-07:01:01  1      x9000c1s1b0n0
12090  192C4G1H_MI300A_RHEL9  skoranne  RUNNING  2-07:40:59  1      x9000c1s0b1n0
12010  192C4G1H_MI300A_RHEL9  skoranne  RUNNING  3-05:09:01  1      x9000c1s0b0n0
```
3 jobs total on the cluster, one user, 3 nodes held for days (5-day limit allows it). 10 of 13 A1 nodes idle; login
load average 0.07. aac6 at the same time: s24-16/26 idle, s24-30 down*, one unrelated SH5 job.

## 6. Comparison table

| Resource | aac6 (PPAC_MI300A_SPX) | aac7 (192C4G1H_MI300A_RHEL9_A1) |
|---|---|---|
| MI300A nodes usable | 2 up (s24-16, s24-26; s24-30 down) | 13 (10 idle now, 3 long-held) |
| APUs per node / total | 4 / 8 usable (12 configured) | 4 / 52 |
| Node RAM (Slurm RealMemory) | 514000 MB | 500000 MB (`free`: 501 GiB) |
| CPU per node | 192 logical | 192 logical |
| Max job time | 8 h (protocol: ≤ 45 min) | **5 days** |
| MaxNodes per job | unlimited (3 physical) | unlimited (13 physical) |
| Inter-node network | 1 GbE | **4 × 200 Gb/s Slingshot-11 (cxi) + 1 GbE mgmt** |
| libfabric provider | tcp | **cxi** (libfabric 2.3.1), also tcp over hsn IPs |
| Slurm | 23.11.11, cluster `prerelease` | 25.11.4, cluster `midgard` |
| OS | Ubuntu 24.04, kernel 6.8 | RHEL 9.6, kernel 5.14.0-570 |
| ROCm | modules 6.3–7.14, default 7.2.4; hipcc only after `module load rocm` | /opt/rocm-7.0.3 on PATH; modules 7.0.3…7.14, 10.0.0 |
| SHMEM | Sandia OpenSHMEM in `~/sos` (our build) | **cray-openshmemx 11.8.0** (`-lsma`), no SOS |
| MPI | (none used) | cray-mpich 9.1.0, OpenMPI 5.0.10 (ofi/ucx) |
| Home FS | `/shared` 67 T, 26 T free | `/shared/midgard/home` 11 T, 3.1 T free — **different FS, empty** |
| Node-local disk | see phase notes | `/tmp` tmpfs 251 GiB; 2 × 3.5 TB NVMe unmounted |
| Login node GPUs | none | none |
| Cross-cluster | port 22 open both ways via public IPs; compute nodes private | same |

## 7. Assessment

1. **Scale**: aac7 gives up to 13 nodes / 52 APUs in one job (10 idle right now) with a 5-day limit — vs 2–3 nodes on
   aac6 — so the multi-node M1+M2 work (on hold per Phase 8) and the 5.1e13 target scaling can be tested at 4, 8, 12
   nodes.
2. **Interconnect**: Slingshot-11, 4 × 200 Gb/s per node via libfabric `cxi`, ~800× aac6's 1 GbE; the xGMI-vs-network
   ratio changes completely, so the push-vs-pull and overlap defaults need re-measuring.
3. **SHMEM path**: no SOS on aac7, but Cray OpenSHMEMX 11.8.0 (`module load cray-dsmml cray-openshmemx`, link `-lsma`,
   launch with `srun`) is a conforming OpenSHMEM 1.5 and should drop in; a SOS build over libfabric/cxi is the
   fallback (would need a build — not done).
4. **aac6+aac7 joint runs are not practical**: separate Slurm clusters, compute nodes on private non-routed subnets,
   only login-node port 22 reachable over the public internet; treat the clusters as independent.
5. **Porting cost**: different home FS (nothing of ours there, 3.1 T free), RHEL 9 + ROCm 7.0.3 default (7.2.4 module
   exists for toolchain parity), hipcc on the login node; a `~/ntt` clone and the 4 × 10¹⁰ reference must be scp'd in
   via the login node; the tmpfs `/tmp` eats node RAM, so staging on disk needs an admin-mounted NVMe or NFS.
