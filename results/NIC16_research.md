# NIC16 — How one process (or one node's process set) can drive all of a node's Slingshot NICs

Research note, 2026-10-05 (Eastern). Web and source research only. Nothing in this note was run on aac7.
Labels used: **[F]** = fact, quoted or read from the cited doc or source; **[I]** = my inference; **[M-ours]** = our own
measurement (C16 / P16).

Starting point (from C16 / P16, **[M-ours]**): one Cray OpenSHMEMX PE per node drives one NIC. Four contexts give 13.5–14.8 GB/s.
`SHMEM_OFI_NUM_NICS=4` gives nothing more. With 4 PEs per node, each PE gets 7.6–8.2 GB/s, about 31–32 GB/s per node. One context
peaks at about 14 GB/s with 4 MiB puts, and 64–256 MiB puts drop to 4.7–7.9 GB/s. `NIC_POLICY=NUMA` is refused because the NIC's
numa node reads −1. A Cassini port's peak is 25 GB/s each way.

---

## 0. Answer in brief

- **[F]** No HPE-supplied library stripes one rank's or PE's traffic over several Cassini NICs. Cray MPICH and Cray OpenSHMEMX both
  document "each rank/PE will be assigned to exactly one NIC". The HPE Frontier training slide says Cray MPI "does not currently (and
  isn't planning on) load balancing a single MPI rank's traffic across multiple Cassini NICs".
- **[F]** Nothing in libfabric's cxi provider or the CXI driver stops one process from opening one `fid_domain` per NIC (`cxi0..cxi3`).
  Under Slurm's `switch/hpe_slingshot` plugin, the job step gets **one CXI service per NIC**. The provider picks the right service
  for each device on its own, from `SLINGSHOT_DEVICES` / `SLINGSHOT_SVC_IDS`, matched by index.
- **[F]** These stacks do drive several NICs from one process:
  1. raw libfabric, with one domain and endpoint per NIC (our own code);
  2. upstream MPICH ch4:ofi (`MPIR_CVAR_CH4_OFI_MAX_NICS`, `..._ENABLE_MULTI_NIC_STRIPING`);
  3. RCCL plus aws-ofi-nccl when one process owns several GPUs (one net device per NIC). The plugin's RDMA protocol also stripes
     over several NICs per GPU and now has `FI_MR_ENDPOINT` code for cxi;
  4. libfabric's `lnx` provider (multi-rail, tagged only, **no HMEM**);
  5. libcxi directly.
- **[I] Most promising:** a raw-libfabric bulk-exchange layer with one cxi domain and endpoint per APU thread, each bound to its own
  NIC. It writes 4 MiB-chunked `fi_write`s from registered HBM (dmabuf) and signals completion with remote MR counters. Cray
  OpenSHMEMX stays for bootstrap, barriers and small traffic. This keeps ecalc's one-process-per-node, four-thread design. It is
  also the only option that maps cleanly onto the target's 2 NICs per APU: each thread opens 2 domains.
- **[I] The fallback with no new transport code:** 4 PEs per node, where PEs 1–3 are thin NIC proxies with small symmetric staging
  windows. PE 0 fills those windows through `shmem_ptr` / hipIpc. This adds no full-size heap per PE (that cost is what sank P16's
  4-PE form), but it inherits SHMEM's ~8 GB/s per PE.

---

## 1. Options, one by one

### (a) Raw libfabric, one domain + endpoint per NIC

| item | finding |
|---|---|
| mechanism | `fi_getinfo(FI_VERSION(2,0), …, hints)` with `hints->fabric_attr->prov_name="cxi"` and `hints->domain_attr->name="cxiN"`, then `fi_fabric` / `fi_domain` / `fi_endpoint(FI_EP_RDM)` / `fi_av_open` / `fi_cq_open` per NIC. **[F]** fi_cxi: "The NIC interface string should match the name of an available CXI domain (in the format cxi[0-9])". Each cxi device appears as its own `fi_info` entry. |
| NIC selection | `domain_attr->name`, or the env `FI_CXI_DEVICE_NAME=cxi0,cxi1,…` (**[F]** "Restrict CXI provider to specific CXI devices"). **[F]** NICs whose retry handler is not running, or whose AMA is not recognised, are dropped from the list (`cxip_if.c`; can be overridden with `CXIP_SKIP_RH_CHECK` / `CXIP_SKIP_AMA_CHECK`). |
| several domains in one process | **[F]** Allowed. The address is (NIC addr, VNI, PID). "All OFI Endpoints sharing a Domain share the same NIC Address", and PIDs 0–510 per NIC are auto-assigned. **[F]** Slurm's plugin loop "Create a Service for each NIC" (`setup_nic.c`) and exports `SLINGSHOT_DEVICES=cxi0,…` and `SLINGSHOT_SVC_IDS=a,b,…` in the same order. The provider's `cxip_nic.c` looks up the device's index in `SLINGSHOT_DEVICES` to find its svc_id. So a domain on cxi2 gets cxi2's service, with the job VNI (the first entry of `SLINGSHOT_VNIS`). |
| GPU memory | **[F]** `FI_HMEM_ROCR` supported, via dmabuf, "ROCm 5.6+". `FI_HMEM_ROCR_USE_DMABUF` is on by default in cxi. Ask for `caps \|= FI_HMEM` and `mr_mode \|= FI_MR_HMEM`, and register with `fi_mr_regattr` using `.iface = FI_HMEM_ROCR`. **[I]** `hipMalloc` buffers should work. `hipMemCreate` (VMM) buffers are **unverified**, because dmabuf export of VMM handles depends on the ROCm version. Test both. The fallback is to register the VMM range as `FI_HMEM_SYSTEM`, since MI300A HBM is CPU-addressable, but that pins pages through `get_user_pages`, which may refuse device pages. |
| MR mode | **[F]** cxi "requires FI_MR_ENDPOINT"; `FI_MR_ALLOCATED` is required unless ODP is on. Client keys 0–99 are "optimized MRs". Use `FI_MR_PROV_KEY` or client keys ≥ 100 for big slabs. With `FI_MR_ENDPOINT`, call `fi_mr_bind(mr, &ep->fid, 0)` and then `fi_mr_enable(mr)`. MRs are **per domain, so per NIC**: one slab used on two NICs needs two registrations (same pages, 2 keys). |
| completion signalling | **[F]** cxi supports MR-bound counters. `fi_mr_bind(mr, &cntr->fid, FI_REMOTE_WRITE)` needs `FI_RMA_EVENT` in caps; the RAMC paper builds on these "memory region counters". `fi_writedata` needs `FI_CXI_ENABLE_WRITEDATA=1`. |
| thread safety | **[F]** `FI_THREAD_SAFE`, `FI_THREAD_COMPLETION`, `FI_THREAD_DOMAIN`. **[I]** One thread per domain with `FI_THREAD_DOMAIN` is the lock-free form and fits ecalc's one-thread-per-APU model. |
| VNI / credentials | **[F]** Auto-keying order: `SLINGSHOT_VNIS/DEVICES/SVC_IDS` env → a service matching the UID → one matching the GID → the first unrestricted service (`FI_CXI_DEFAULT_VNI`, default 10). Without an srun step (no `SLINGSHOT_*` env) only the default service is tried. Slurm services have `restricted_members=true` (your UID only). |
| −FI_ENOSPC (−28) causes | **[F]** In the CXI driver, `-ENOSPC` comes from: (1) `cass_rgroup.c`, where a resource's `in_use ≥ max` with no shared pool left, or a service asked for `reserved > shared available` / `max > device max`; (2) `cass_rgid.c`, no free RGID, since each LNI (≈ each libfabric domain) needs an RGID unless shared through `lnis_per_rgid`; (3) `cass_svc.c`, service creation over budget. **[F]** Slurm sizes each NIC's service as `reserved = def_per_thread × step_cpus` (or `--network=depth=N`) and `max = device max`, **except TLEs, where `max = reserved`** (defaults per CPU: 2 txqs, 1 tgq, 2 eqs, 1 ct, **1 tle**, 6 ptes, 16 les, 2 acs). **[F]** In libfabric, triggered ops return `-FI_ENOSPC` when `FI_CXI_ENABLE_TRIG_OP_LIMIT` is set and TLEs run out. **[I]** For one process with 4 domains, the likely ENOSPC culprits are a step with few CPUs (e.g. `-c 1` → 1 TLE, 6 PTEs, 2 EQs per NIC) when the library also uses triggered ops or collectives, or a NIC-wide pool held by leaked services of earlier jobs. Diagnose with `cxi_service list -v -d cxiN` (limits and in_use), `FI_LOG_LEVEL=warn FI_LOG_PROV=cxi`, `SlurmdDebug` flag `SWITCH`. Fix with `srun -c <all cores>` or `--network=def_tles=…,def_ptes=…,def_eqs=…`. |
| bandwidth reported | **[F]** Alps/LUMI GPU-aware MPI between nodes reaches ≈ 95 % of 200 Gb/s (≈ 23.7 GB/s) per NIC, one process per GPU/NIC (arXiv 2408.14090). **[F]** Aurora (Cassini, 4 NICs per socket): host buffers ≈ 90 GB/s per socket and GPU buffers ≈ 70 GB/s per socket with one process per NIC. "The NICs cannot be saturated by one process per NIC", and two processes per NIC did better (arXiv 2512.04291). No published single-process, 4-domain number was found. |
| pitfalls | **[I]** Each extra domain adds its own CQs, EQs and PTEs (see limits). AV addresses are per NIC, so peer address exchange is 4× (node × NIC). Our own large-put slump (64–256 MiB at 4.7–7.9 GB/s, **[M-ours]**) argues for 1–8 MiB `fi_write`s with 8–32 outstanding, whatever the backend. **[F]** cxi's default `FI_CXI_RDZV_THRESHOLD` is 16 KiB; this matters only for msg/tagged, not RMA. |

### (b) Cray OpenSHMEMX (current)

- **[F]** `SHMEM_OFI_NUM_NICS`: "number of NICs the job can use on a per-node basis". The optional index form is `"2:1,3"`.
  `SHMEM_OFI_NIC_POLICY` = BLOCK (default) | ROUND-ROBIN | NUMA | USER, and "**Each OpenSHMEM PE will be assigned to exactly one
  NIC**". `SHMEM_OFI_NIC_MAPPING="0:0-7; 2:8-31"` applies only with USER. Others: `SHMEM_OFI_USE_DOMAIN_NAME`, `SHMEM_OFI_USE_PROV_NAME`,
  `SHMEM_OFI_PROVIDER_DISPLAY`, `SHMEM_OFI_USE_SEP`, `SHMEM_MAX_CTX`. Contexts and teams are not documented to map to different NICs.
- **[F]** Multi-NIC support arrived in 11.1.0, as "multiple NICs per node". The GPU-aware design (CUG 2025) adds `SHMEM_SPACE_DEVICE`,
  `SHMEM_SYMMETRIC_SIZE_DEVICE` and `SHMEM_ENABLE_DEVICE_AWARENESS`, and was evaluated on El Capitan-class nodes with 4 PEs per node.
- **Why one PE uses one NIC:** **[F]** this is the documented design, and **[M-ours]** it is confirmed by C16. `NUM_NICS=4` with one PE
  gives the same 13.5–14.8 GB/s. **[I]** Every context of a PE opens its endpoint on the PE's single domain. No env variable switches
  this, so there is nothing more to try here except the proxy pattern in (e).
- Pitfall, **[M-ours]**: `NIC_POLICY=NUMA` dies on aac7 ("numa node -1"). Use BLOCK or USER mapping.

### (c) HPE Cray MPICH

- **[F]** `MPICH_OFI_NIC_POLICY` = BLOCK (default) | ROUND-ROBIN | NUMA | GPU | USER; "Each MPI rank will be assigned to exactly one
  NIC". `MPICH_OFI_NIC_MAPPING`, `MPICH_OFI_NUM_NICS="n:idx,…"`, and `MPICH_OFI_NIC_VERBOSE=2` (prints each rank's NIC). GPU picks
  "the NIC that is closest to the GPU selected by the user".
- **[F]** No striping across NICs from one rank: "isn't planning on" doing it (OLCF/HPE 2023 slide). CUG 2023 lists it as "under
  evaluation".
- **[F]** Big Send-off (arXiv 2504.18658) saw Cray MPICH collectives constrain "all read operations to NIC 3 and all write operations
  to NIC 0". RCCL spread more evenly.
- Use for us: **[I]** none beyond what SHMEM already gives.

### (c′) Upstream MPICH ch4:ofi (not Cray's build) — does stripe from one rank

- **[F]** CVARs in `src/mpid/ch4/netmod/ofi/ofi_init.c`, with their defaults:
  - `MPIR_CVAR_CH4_OFI_MAX_NICS=1`; `-1` means auto, up to `MPIDI_OFI_MAX_NICS=128`;
  - `MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_STRIPING=0`: "enables striping of large messages across multiple NICs";
  - `MPIR_CVAR_CH4_OFI_MULTI_NIC_STRIPING_THRESHOLD=1048576`;
  - `MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_HASHING=0`: different NICs per message or peer;
  - `MPIR_CVAR_CH4_OFI_PREF_NIC`;
  - a per-communicator `multi_nic_pref_nic` info hint.

  NIC "closeness" comes from PCI/hwloc, or from the GPU (`try_set_nic_close_by_gpu`).
- **[F]** It is used on Aurora (cxi, 8 NICs per node). The Aurora HPL paper turned striping and hashing *off* there for determinism
  and steered phases with `multi_nic_pref_nic`.
- GPU memory: **[I]** striping runs on the large-message (huge / RMA-read) tagged path. That path should accept ROCm buffers when MPICH
  is built with `--with-device=ch4:ofi --with-libfabric=<2.3.1> --enable-gpu` / HIP, but this is unverified on cxi + MI300A.
- Pitfalls, **[I]**: a self-built MPICH needs a PMI that srun or PALS provides (`--with-pmi=pmi2` or `pmix`). It cannot share a
  process with Cray OpenSHMEMX's libfabric unless both use the same libfabric 2.3.1. It is two-sided (send/recv), not SHMEM puts.

### (d) RCCL + aws-ofi-nccl (aws-ofi-rccl is deprecated)

- **[F]** `ROCm/aws-ofi-rccl` says it is "deprecated, please utilize upstream" `aws/aws-ofi-nccl`, which now has ROCm support.
- **[F]** The plugin has two protocols. SENDRECV is one NIC per NCCL net device. RDMA "builds rails" across NICs and stripes with
  `fi_write` (`OFI_NCCL_FORCE_NUM_RAILS`). Selection: if the user sets `OFI_NCCL_PROTOCOL`, that is used. Otherwise RDMA is used
  "if the rdma protocol reports multiple nics per device", and SENDRECV in all other cases. Current master has `FI_MR_ENDPOINT`
  handling in `nccl_ofi_rdma.cpp` (per-rail MR registration), which cxi needs. Issue #1042 (Nov 2025) proposed it, and v1.17.2 fixed
  a shutdown bug "on NICs that require per-endpoint memory registration (Cray Slingshot)". `OFI_NCCL_NIC_DUP_CONNS=N` duplicates
  each NIC as N devices.
- **[F]** Recommended Slingshot env (CSCS):
  ```
  NCCL_NET="AWS Libfabric"  NCCL_NET_GDR_LEVEL=PHB  NCCL_CROSS_NIC=1  NCCL_PROTO=^LL128
  FI_CXI_DEFAULT_CQ_SIZE=131072  FI_CXI_DEFAULT_TX_SIZE=16384
  FI_CXI_DISABLE_HOST_REGISTER=1  FI_CXI_RX_MATCH_MODE=software
  FI_MR_CACHE_MONITOR=userfaultfd  NCCL_NCHANNELS_PER_NET_PEER=4
  ```
  Big Send-off on Frontier also used `FI_CXI_RDZV_THRESHOLD=0 FI_CXI_RDZV_GET_MIN=0 FI_CXI_EAGER_SIZE=0` and `HSA_ENABLE_SDMA=0`.
- One process, all NICs: **[I]** RCCL allows one process to own all 4 GPUs (`ncclCommInitAll`, or one `ncclCommInitRank` per GPU
  inside `ncclGroupStart/End`). Each GPU's communicator then uses its nearest NIC through the plugin, so one process drives 4 NICs
  with HBM buffers and GDR. On the target, with 2 NICs per APU, the RDMA protocol's rails could stripe each GPU's traffic over both.
- Reported numbers, **[F]**: RCCL is ≈ 4× Cray MPICH on bandwidth-bound allgather on Frontier (Big Send-off). *CCL reaches ≈ 75 %
  efficiency on large allreduce (2408.14090). No MI300A per-node all-to-all number was found.
- Pitfalls, **[I]**: RCCL's all-to-all runs as GPU kernels plus proxy threads and needs CUs or channels, which competes with our
  NTT kernels. Buffers must be RCCL-registered or cached. VMM buffers need `ncclCommRegister` and may fall back. Is aws-ofi-nccl
  built for ROCm on aac7? (Check `module avail` for `aws-ofi-*` / `rccl-net-plugin`.)

### (e) Several PEs per node as NIC proxies (shared memory)

- **[M-ours]** 4 PEs per node reach ≈ 31–32 GB/s per node. The obstacle in P16 was memory (4 × full heaps), not bandwidth.
- Design, **[I]**: PE 0 per node runs ecalc as today (4 APU threads). PEs 1..3 are proxies: no compute, a small symmetric heap, a
  staging ring of K × 4 MiB per peer, and their own NIC (BLOCK or USER mapping, 4 PEs → cxi0..3). PE 0's thread *t* copies its
  outgoing slab into proxy *t*'s window, pointed to by `shmem_ptr(win, proxy_pe)`. Intra-node `shmem_ptr` works through XPMEM in
  Cray OpenSHMEMX (**[F]** XPMEM is an intra-node transport module, CUG 2022). The copy runs at HBM / xGMI speed, far above 25 GB/s.
  The proxy then does `shmem_putmem_signal_nbi` to the remote proxy's window. On the receive side, PE 0 reads the bytes out of the
  local proxy's window.
- Alternatives for the shared buffer, **[I]**: `hipIpcGetMemHandle` / `hipIpcOpenMemHandle` (plain `hipMalloc`), or
  `hipMemExportToShareableHandle(…, hipMemHandleTypePosixFileDescriptor)` with an fd passed over a UNIX socket and
  `hipMemImportFromShareableHandle` + `hipMemMap` (VMM). Then register the imported range in the proxy's *raw libfabric* domain. A
  SHMEM symmetric heap cannot adopt external memory, so for pure SHMEM proxies a copy is needed.
- Cost: one extra on-node copy per byte each way, and the ~8 GB/s per PE SHMEM ceiling (**[M-ours]**). Gains: no transport rewrite,
  and the heaps stay small.

### (f) Sandia OpenSHMEM (SOS) over cxi

- **[F]** SOS 1.5.3 "Added support for multi-NIC configurations via libfabric", on by default and disabled with
  `SHMEM_OFI_DISABLE_MULTIRAIL=1`. **[F]** Source (`transport_ofi.c`): it gathers one `fi_info` per distinct NIC, then
  `assign_nic_with_hwloc` or `provs[my_pe % num_nics]` picks **one** NIC per PE. So it is the same one-NIC-per-PE model as Cray's.
  `SHMEM_OFI_DOMAIN=cxi2` pins it, and the build flags are `--enable-ofi-inject --enable-nonfetch-amo`.
- **[I]** No gain over Cray OpenSHMEMX for this question.

### (g) UCX

- **[F/I]** There is no UCX transport for Cassini. The only routes are TCP over the `hsn*` Ethernet netdevs (slow) or rebuilding the
  stack around libfabric. Not viable.

### (h) Others

- **libfabric `lnx` (LINKx) provider**, **[F]**: `FI_PROVIDER=lnx`, `FI_LNX_PROV_LINKS="cxi:cxi0,cxi1,cxi2,cxi3"` (or
  `shm+cxi:cxi0,…`), and `FI_LNX_MULTI_RAIL_SELECTION=PER_PEER` (default) or `PER_MSG`. Round-robin over domains. "Tagged
  operations only", `FI_EP_RDM`. "Hardware offloads (tag matching, **HMEM**) aren't supported". Marked experimental; CSCS uses it
  with Open MPI (`OMPI_MCA_mtl_ofi_av=table`, and async collectives on GPU buffers segfault). **[I]** Usable only with host-pinned
  staging. Under PER_PEER a single peer pair still uses one rail, so PER_MSG is required for one-pair bandwidth.
- **libcxi directly**, **[F]**: `cxil_open_device`, `cxil_alloc_lni(dev, svc_id)`, `cxil_alloc_cp(vni)`, `cxil_alloc_cmdq`, and
  `cxil_map`. HPE's `cxi_write_bw` / `cxi_read_bw` / `cxi_send_bw` take `-d cxiN -v SVC_ID --tx-gpu=G --rx-gpu=G -g AMD -s MIN:MAX
  -l LIST -b --use-hp=2M`, and the user must be in group `video` for GPU buffers. **[I]** Use these as the per-NIC ceiling probe, not
  for production.
- **Open MPI 5 (mtl/ofi or btl/ofi on cxi)**, **[I]**: one NIC per process by default; multi-NIC only through `lnx`.
- **rocSHMEM**, **[F]**: the GDA backend supports mlx5, bnxt_re and Pensando ionic, not Cassini. The reverse-offload backend rides on
  MPI. **[I]** Not a route to more NICs.
- **HPE "multi-rail"**, **[F/I]**: no HPE RDMA bonding exists. Slingshot Host Software exposes `hsn0..3` netdevs (Ethernet / TCP
  only) and one cxi device per NIC. "Multi-rail" in HPE docs means multiple NICs per node with one-NIC-per-rank assignment.

---

## 2. Ranked experiments for aac7

All runs use ≤ 2–4 aac7 nodes inside an `srun` step, so the `SLINGSHOT_*` env exists. Start every run by recording
`env | grep -E 'SLINGSHOT|FI_|SHMEM_'`, `fi_info -p cxi -l`, and `cxi_service list -v` for each device.

### E1 — Per-NIC ceiling, all 4 NICs at once, with HBM (libcxi tools; ½ day)

Why: it tells us whether 4 × ≈ 23 GB/s is reachable at all from HBM on MI300A, before any code is written.
```
# inside: srun -N2 -n2 --ntasks-per-node=1 -c <all> --pty bash  (or a script)
SVC=($(echo $SLINGSHOT_SVC_IDS | tr , ' '))
# server node: for d in 0 1 2 3: cxi_write_bw -d cxi$d -v ${SVC[$d]} -p $((49000+d)) --rx-gpu=$d -g AMD &
# client node: for d in 0 1 2 3: cxi_write_bw <srv_nic_addr_d> -d cxi$d -v ${SVC[$d]} -p $((49000+d)) \
#              --tx-gpu=$d -g AMD -s 1048576:268435456 -l 16 -D 10 &
```
Arms: 1 NIC alone; 4 NICs together; host buffers vs `--tx-gpu`; GPU index = NIC index vs crossed. The server address is the NIC's
address (`cxi_stat -d cxiN`). If `-v` with the Slurm svc fails, try the default svc 1 outside Slurm (that needs the default service
to be enabled). **Pass:** ≥ 20 GB/s per NIC alone and ≥ 80 GB/s summed.

### E2 — One process, 4 threads, 4 cxi domains, raw libfabric RMA (1–2 days) — main candidate

Bootstrap with Cray OpenSHMEMX, already linked: it exchanges address blobs and keys. Same libfabric 2.3.1 module.
```c
/* nic4_bw.c — per thread t (one per APU / NIC) */
hipSetDevice(t);
struct fi_info *h = fi_allocinfo(), *fi;
h->fabric_attr->prov_name = strdup("cxi");
h->domain_attr->name      = strdup(nic_name[t]);       /* "cxi0".."cxi3" (aac7); target: 2 per APU */
h->caps = FI_RMA | FI_HMEM | FI_RMA_EVENT;
h->ep_attr->type = FI_EP_RDM;
h->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR | FI_MR_HMEM | FI_MR_LOCAL;
h->domain_attr->threading = FI_THREAD_DOMAIN;
fi_getinfo(FI_VERSION(2,0), NULL, NULL, 0, h, &fi);   /* print fi->domain_attr->name to verify */
fi_fabric(fi->fabric_attr, &fab, NULL); fi_domain(fab, fi, &dom, NULL);
fi_cq_open(dom, &(struct fi_cq_attr){.format=FI_CQ_FORMAT_CONTEXT,.size=4096}, &cq, NULL);
fi_av_open(dom, &(struct fi_av_attr){.type=FI_AV_TABLE}, &av, NULL);
fi_cntr_open(dom, &(struct fi_cntr_attr){.events=FI_CNTR_EVENTS_COMP}, &rcntr, NULL);
fi_endpoint(dom, fi, &ep, NULL);
fi_ep_bind(ep, &cq->fid, FI_TRANSMIT); fi_ep_bind(ep, &av->fid, 0); fi_enable(ep);
fi_getname(&ep->fid, myaddr, &alen);                   /* -> shmem symmetric table [pe][t] */
/* buffer: hipMalloc (arm A) or hipMemCreate+hipMemMap (arm B), 1 GiB */
struct iovec iov = {buf, len};
struct fi_mr_attr ma = {.mr_iov=&iov,.iov_count=1,.access=FI_WRITE|FI_REMOTE_WRITE,
                        .iface=FI_HMEM_ROCR,.device.reserved=t};
fi_mr_regattr(dom, &ma, 0, &mr); fi_mr_bind(mr, &ep->fid, 0);
fi_mr_bind(mr, &rcntr->fid, FI_REMOTE_WRITE); fi_mr_enable(mr);
key = fi_mr_key(mr);                                   /* -> shmem table with (vaddr,key) */
shmem_barrier_all(); fi_av_insert(av, peer_addr[peer][t], 1, &fiaddr, 0, NULL);
/* timed loop: CH=4 MiB chunks, depth Q=16 */
for (off=0; off<len; off+=CH) { while (inflight==Q) drain(cq);
  fi_write(ep, buf+off, CH, fi_mr_desc(mr), fiaddr, raddr+off, rkey, ctx); inflight++; }
drain_all(cq);   /* receiver: fi_cntr_wait(rcntr, nchunks_expected, -1) */
```
Arms:
- threads 1 / 2 / 4, with thread t on cxi t (expected ≈ 4× scaling), plus all 4 threads on cxi0 (the control);
- chunk 256 KiB / 1 / 4 / 16 / 64 MiB × Q 4 / 16 / 64;
- `hipMalloc` vs VMM vs host-pinned;
- `FI_HMEM_ROCR_USE_DMABUF=1` (default) vs 0;
- bidirectional;
- 2 nodes, then 4 nodes all-to-all.

Env:
```
FI_PROVIDER=cxi FI_LOG_LEVEL=warn FI_LOG_PROV=cxi FI_CXI_DEFAULT_CQ_SIZE=131072
FI_MR_CACHE_MONITOR=userfaultfd (or memhooks)   SHMEM_OFI_NIC_POLICY=BLOCK (SHMEM stays on cxi0)
```
If ENOSPC appears: rerun with `srun -c <all cores>` (Slurm reserves per CPU) and read `cxi_service list -v` in_use / max.

**Pass:** 4 threads ≥ 3.5× one thread, and ≥ 60 GB/s per node.

### E3 — Integrate as ecalc `comm_ofi` bulk path (after E2 passes; 3–5 days)

`comm_alltoall` slabs go through E2's per-thread domain: 4 MiB `fi_write` stream plus one remote-counter target per (thread, round).
SHMEM keeps the barriers, the small control traffic and the bootstrap. MRs are registered once per pool slab, for each NIC that will
touch it (per-NIC keys). On the target, each thread opens 2 domains (its two NICs) and alternates chunks between them. Verify that
the digits are identical to GMP at 10⁹–10¹⁰, then compare `dm` and `t_comm` against C16.

### E4 — RCCL single-process 4-GPU all-to-all (1 day if the plugin exists)

```
module load rocm rccl aws-ofi-nccl   # or build aws/aws-ofi-nccl --with-rocm --with-libfabric
NCCL_NET="AWS Libfabric" NCCL_NET_GDR_LEVEL=3 NCCL_CROSS_NIC=1 NCCL_PROTO=^LL128 \
FI_CXI_DISABLE_HOST_REGISTER=1 FI_MR_CACHE_MONITOR=userfaultfd FI_CXI_DEFAULT_CQ_SIZE=131072 \
FI_CXI_DEFAULT_TX_SIZE=16384 FI_CXI_RX_MATCH_MODE=software NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET \
srun -N2 --ntasks-per-node=1 -c <all> ./alltoall_perf -g 4 -b 4M -e 1G -f 2
```
Check in the log: 4 net devices `cxi0..3`, one per GPU, and "Using transport protocol". Compare per-node GB/s with E2. Mostly a
yardstick, since RCCL competes with our kernels for CUs.

### E5 — 4 PEs per node, PEs 1–3 as NIC proxies with small windows (2–3 days)

Use this if E2 fails, for example on HMEM registration of VMM or ENOSPC that cannot be fixed. `SHMEM_SYMMETRIC_SIZE` is small for
the proxies. Test first with `t_comm` in 4-PE pairs: an `shmem_ptr` copy into the proxy window, then `putmem_signal_nbi` at 4 MiB
chunks. Expected ≈ 31 GB/s per node (**[M-ours]** per-PE rates). Memory cost ≈ 3 × (window × peers).

### E6 — Upstream MPICH striping from one rank (2–3 days, low priority)

Build MPICH 4.3+ `--with-device=ch4:ofi --with-libfabric=$CRAY_LIBFABRIC_PREFIX --with-pmi=pmi2 --with-hip`, then run osu `osu_bw -d
rocm D D` with 1 rank per node and these env settings:
```
MPIR_CVAR_CH4_OFI_MAX_NICS=4 MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_STRIPING=1
MPIR_CVAR_CH4_OFI_MULTI_NIC_STRIPING_THRESHOLD=1048576 MPIR_CVAR_CH4_OFI_ENABLE_MULTI_NIC_HASHING=1
MPIR_CVAR_ENABLE_GPU=1 FI_PROVIDER=cxi
```
This shows whether one-rank striping over cxi works with HBM. It would mean replacing SHMEM with two-sided MPI, which is a big change.

### E7 — lnx multi-rail (½ day, informational)

```
FI_PROVIDER=lnx FI_LNX_PROV_LINKS="cxi:cxi0,cxi1,cxi2,cxi3" FI_LNX_MULTI_RAIL_SELECTION=PER_MSG
```
Run with fabtests `fi_rdm_tagged_bw` on host buffers. Only meaningful for a host-staged design; no HMEM.

Not recommended: SOS (one NIC per PE), UCX (no cxi), more `SHMEM_OFI_*` knobs (documented one-NIC-per-PE), rocSHMEM GDA (no Cassini).

---

## 3. Sources

- fi_cxi(7): https://ofiwg.github.io/libfabric/main/man/fi_cxi.7.html , https://ofiwg.github.io/libfabric/v2.1.0/man/fi_cxi.7.html
- cxi provider source: https://github.com/ofiwg/libfabric/tree/main/prov/cxi/src (`cxip_nic.c` SLINGSHOT_* parsing, `cxip_info.c` `cxip_gen_auth_key`, `cxip_if.c` RH/AMA checks, `cxip_mr.c` MR counters)
- fi_lnx(7): https://ofiwg.github.io/libfabric/main/man/fi_lnx.7.html , https://man.archlinux.org/man/extra/libfabric/fi_lnx.7.en
- Slurm switch/hpe_slingshot source: https://github.com/SchedMD/slurm/tree/master/src/plugins/switch/hpe_slingshot (`setup_nic.c`, `switch_hpe_slingshot.h`)
- CXI driver (ENOSPC paths): https://github.com/HewlettPackard/shs-cxi-driver (`cass_rgroup.c`, `cass_rgid.c`, `cass_svc.c`)
- libcxi utils: https://github.com/HewlettPackard/shs-libcxi/tree/main/utils (`write_bw.c` usage); HPE CXI diags: https://support.hpe.com/hpesc/public/docDisplay?docId=dp00005009en_us&page=troubleshoot%2Fcxi%2Fcxi_diags_overview.html
- Cray OpenSHMEMX intro_shmem: https://cray-openshmemx.readthedocs.io/en/latest/intro_shmem.html , https://cpe.ext.hpe.com/docs/24.07/mpt/openshmemx/intro_shmem.html ; release notes: https://cray-openshmemx.readthedocs.io/en/latest/release_notes.html
- CUG 2022 OpenSHMEM on Slingshot 11: https://cug.org/proceedings/cug2022_proceedings/includes/files/pap111s2-file1.pdf
- CUG 2025 GPU-aware OpenSHMEMX: https://cug.org/proceedings/cug2025_proceedings/includes/files/pap106s2-file1.pdf
- Cray MPICH intro_mpi: https://cpe.ext.hpe.com/docs/24.03/mpt/mpich/intro_mpi.html
- HPE/OLCF "Getting the most out of HPE Cray MPI" (no per-rank load balancing): https://www.olcf.ornl.gov/wp-content/uploads/20230216_HPE_Cray_MPI.pdf
- CUG 2023 HPE Cray MPT design (multi-NIC per process "under evaluation"): https://cug.org/proceedings/cug2023_proceedings/includes/files/pap144s2-file1.pdf
- Upstream MPICH ch4:ofi multi-NIC CVARs: https://github.com/pmodels/mpich/blob/main/src/mpid/ch4/netmod/ofi/ofi_init.c , `ofi_nic.c`
- Aurora HPL (striping/hashing off, pref_nic): https://arxiv.org/pdf/2604.09517 ; Aurora MPI scaling (per-socket 90/70 GB/s): https://arxiv.org/pdf/2512.04291
- aws-ofi-nccl: https://github.com/aws/aws-ofi-nccl (`include/nccl_ofi_param.h`, `src/nccl_ofi_net.cpp`, `src/nccl_ofi_rdma.cpp`), issue #1042: https://github.com/aws/aws-ofi-nccl/issues/1042 ; deprecated fork: https://github.com/rocm/aws-ofi-rccl
- CSCS NCCL on Slingshot: https://docs.cscs.ch/software/communication/nccl/ ; CSCS Open MPI + LNX: https://docs.cscs.ch/software/communication/openmpi/
- Big Send-off (Frontier RCCL vs Cray MPICH, NIC use): https://arxiv.org/html/2504.18658v1
- GPU-to-GPU interconnect study (95 % of 200 Gb/s): https://arxiv.org/pdf/2408.14090
- RAMC over Slingshot (MR counters): https://arxiv.org/pdf/2606.05094
- Sandia OpenSHMEM: https://github.com/Sandia-OpenSHMEM/SOS/releases , source `src/transport_ofi.c`; wiki https://github.com/Sandia-OpenSHMEM/SOS/wiki/Performance-Tuning
- rocSHMEM: https://rocm.docs.amd.com/projects/rocSHMEM/en/latest/install.html
- Frontier user guide: https://docs.olcf.ornl.gov/systems/frontier_user_guide.html ; El Capitan hardware: https://hpc.llnl.gov/documentation/user-guides/using-el-capitan-systems/hardware-overview
