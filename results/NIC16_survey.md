# aac7 NIC / SHMEM / interconnect survey (read-only) — 2026-10-05, ~21:00–21:45 EDT

Goal: find out whether/how a single process on an aac7 node can drive all 4 Slingshot NICs at once (today, one
Cray OpenSHMEMX PE binds exactly one NIC). Login `chcoppola@aac7.amd.com` (uan1). Compute-node facts taken from the
held job 12287 (`srun --jobid=12287 --overlap -N1 -n1 -w x9000c1s3b0n0 <cmd>`, read-only commands only; nothing
cancelled, no GPU memory/kernels touched beyond `rocm-smi --showtopo`). All values **measured** unless marked
**modelled**/**assumed**. See also `results/AAC7_survey.md` (2026-10-03, general cluster comparison) for Slurm
partitions, filesystems, ROCm modules — not repeated here except where it bears on NICs.

## 1. Hardware

Node used: `x9000c1s3b0n0` (one of the 10 idle A1 nodes under hold job 12287).

### 1.1 PCI / cxi devices

```
lspci:
0000:01:00.0 Ethernet controller: Cray Inc Cassini 1 [Slingshot 200Gb] (rev 02)
0001:01:00.0 Ethernet controller: Cray Inc Cassini 1 [Slingshot 200Gb] (rev 02)
0002:01:00.0 Ethernet controller: Cray Inc Cassini 1 [Slingshot 200Gb] (rev 02)
0003:01:00.0 Ethernet controller: Cray Inc Cassini 1 [Slingshot 200Gb] (rev 02)
```
Each Cassini NIC lives in its own PCI **domain** (0000/0001/0002/0003), not just a different bus — i.e. each socket
has its own root complex with one Cassini attached directly (`0000:00:01.1 -> 0000:01:00.0` etc., via
`/sys/class/cxi/cxiN -> ../../devices/pci000N:00/000N:00:01.1/000N:01:00.0/cxi/cxiN`).

`/sys/class/cxi/cxi{0,1,2,3}/device/numa_node` = `0,1,2,3` respectively — **one Cassini NIC per NUMA node/socket**,
confirming the task's "one NIC per APU" layout.

### 1.2 `cxi_stat` (all 4 identical except serial/MAC/NID)

| Device | hsn | MAC | NID | FW | PCIe | Link speed | Link state |
|---|---|---|---|---|---|---|---|
| cxi0 | hsn0 | 02:00:00:00:08:e1 | 2273 (0x8e1) | 1.5.61-ESM | 20.0 GT/s x16 (PCIe4 ×16) | BS_200G | up |
| cxi1 | hsn1 | 02:00:00:00:08:e0 | 2272 (0x8e0) | 1.5.61-ESM | 20.0 GT/s x16 | BS_200G | up |
| cxi2 | hsn2 | 02:00:00:00:08:21 | — | 1.5.61-ESM | 20.0 GT/s x16 | BS_200G | up |
| cxi3 | hsn3 | 02:00:00:00:08:20 | — | 1.5.61-ESM | 20.0 GT/s x16 | BS_200G | up |

Part number P43012-005 ("SS11 200Gb 2P"), PID granule 256, link layer retry on, media electrical, MTU 2112
(LLDP/Slingshot frame, not 1500/9000 Ethernet MTU). `ip -br link`: `hsn0..hsn3` all `UP`,
`<BROADCAST,MULTICAST,UP,LOWER_UP>`, same MACs as `cxi_stat`. 4 × 200 Gb/s = 800 Gb/s injection per node (as noted in
the 2026-10-03 survey).

### 1.3 NUMA layout (`numactl -H`, node x9000c1s3b0n0)

4 NUMA nodes, 48 logical CPUs each (24 cores × 2 SMT), ~128 GB each (one per MI300A socket — APU-local memory is
part of that node's NUMA domain, consistent with MI300A's unified CPU+GPU package). Node distances: local=10,
remote=32 (uniform — flat distance across all 4 sockets, no closer/farther pairs).

### 1.4 GPU↔NIC affinity (`rocm-smi --showtopo`, requires `--gpus=4` in the srun or it reports no GPUs)

```
GPU[0] Numa Node: 0   GPU[1] Numa Node: 1   GPU[2] Numa Node: 2   GPU[3] Numa Node: 3
```
All GPU-GPU links are XGMI, 1 hop, weight 15 (fully connected mesh, as already known from aac6 MI300A work). Combined
with §1.1's NIC NUMA nodes: **GPU0↔cxi0, GPU1↔cxi1, GPU2↔cxi2, GPU3↔cxi3** — a clean 1:1 socket-local pairing, no
cross-socket NIC sharing. `lstopo-no-graphics` exists (`/usr/bin/lstopo-no-graphics`) but was not run (not in the
quick-command allowlist and not needed given the NUMA data above).

## 2. Kernel / driver

`lsmod | grep -iE "cxi|kdreg|amdgpu"`:
```
cxi_eth, cxi_user, cxi_ss1 (main driver, 1.4 MB), cxi_sl, cxi_sbl   — no separate per-NIC module, one set of
  modules drives all 4 /dev/cxiN minor devices
amdgpu + its amd*/drm* helper modules (ttm, buddy, xcp, kcl, drm_kms_helper, …) — standard ROCm 7.0.3 stack
```
No `kdreg2` module loaded and **no `/dev/kdreg2`** node present (the libfabric `FI_MR_CACHE_MONITOR=kdreg2` option
is therefore not usable as-is on this node image; `userfaultfd` is the default monitor per the libfabric `--env`
dump, §3).

`/dev/cxi0..3`: `crw-rw-rw-` (world RW, major 235, minors 0-3) — any user process can open any of the 4 devices
directly, no special capability needed. `/dev/cxi_sbl` is root-only (`crw-------`, major 237) — the "soft link"
control device, not for applications.

`/sys/module/cxi_ss1/parameters/` has ~40+ tunables (`default_vnis=1,10,0,0`, `default_lnis_per_rgid=1`,
`default_svc_num_tles=512`, `device_hugepage_enable=Y`, `ac_size_sgl=8`, `active_qos_profile=2`, etc.) — module-wide,
not per-NIC (one module instance backs all 4 cxiN devices).

`cxi_service list` (run on cxi0): one service `ID 1 (DEFAULT)`, `LNIs/RGID: 1`, `Enabled: Yes`, `System Service: No`,
`Restricted Members/TCs: No`, resource limits yes. No VNI-service detail was printed in the truncated listing; this
is the per-device resource-group/VNI admission control Slurm's Slingshot plugin also touches (§7).

## 3. libfabric

- **Only one libfabric module**: `libfabric/2.3.1` (default), at `/opt/cray/libfabric/2.3.1`
  (bin/lib64/include/man under there; `LD_LIBRARY_PATH` and `PKG_CONFIG_PATH` prepended by the module). No other
  libfabric tree was found under `/opt` in the paths checked.
- `fi_info -l` (login node, no cxi hardware there) lists providers: `cxi, ofi_rxm, ofi_rxd, shm, udp, tcp,
  ofi_hook_debug, ofi_hook_noop, ofi_hook_hmem, ofi_hook_dmabuf_peer_mem, off_coll, sm2, lnx`. `fi_info -p cxi` on
  the **login** node returns `fi_getinfo: -61 (No data available)` — cxi needs the Cassini hardware, so the
  provider only resolves on compute nodes.
- On the **compute** node, `fi_info -p cxi` enumerates **4 independent domains, one per device**:
  `domain: cxi0/cxi1/cxi2/cxi3`, each `type: FI_EP_RDM`, `protocol: FI_PROTO_CXI` — i.e. at the raw libfabric level
  nothing stops a single process from opening up to 4 domains/endpoints (one per cxiN) and driving all 4 NICs
  itself; the restriction to "one NIC per process" is a **SHMEM/MPI runtime policy**, not a libfabric/hardware
  limit (see §4/§5).
- `fi_info -p cxi -v` summary (domain cxi0, representative of all 4):
  - caps: `FI_MSG, FI_RMA, FI_TAGGED, FI_ATOMIC, FI_COLLECTIVE, FI_READ, FI_WRITE, FI_RECV, FI_SEND,
    FI_REMOTE_READ, FI_REMOTE_WRITE, FI_MULTI_RECV, FI_TRIGGER, FI_FENCE, FI_LOCAL_COMM, FI_REMOTE_COMM,
    FI_RMA_EVENT, FI_NAMED_RX_CTX, FI_DIRECTED_RECV, FI_AV_USER_ID, FI_PEER, FI_HMEM`
  - **`FI_HMEM` is present** — GPU (ROCr) device memory can be registered/transferred directly, no staging
    through host memory required.
  - `mr_mode: [FI_MR_ALLOCATED, FI_MR_PROV_KEY, FI_MR_ENDPOINT]`; `addr_format: FI_ADDR_CXI`; `mr_key_size: 8`;
    `cq_cnt: 32`; `ep_cnt: 128`; `tx_ctx_cnt/rx_ctx_cnt: 1` (per domain, scalable via multiple domains/EPs, not SEP
    by default); `progress: FI_PROGRESS_MANUAL`; `threading: FI_THREAD_SAFE`.
  - `nic.fi_device_attr`: `device_id: 0x501`, `vendor_id: 0x17db` (Cray/HPE), `driver: cxi_ss1`.
    `fi_pci_attr`: `domain_id:0, bus_id:1, device_id:0, function_id:0` (matches lspci's `000N:01:00.0`).
    `fi_link_attr.speed: 200000000000` (200 Gb/s), `mtu: 2112`.
  - `fi_cxi_ext.h` is present under `/opt/cray/libfabric/2.3.1/include/rdma/` — the CXI-provider extension header
    (not inspected further; likely the hook for multi-rail/counter/telemetry extensions used by Cray-internal
    tools).
- `fi_info --env` on the compute node (446 lines) covers generic OFI vars (`FI_LOG_*`, `FI_HMEM*`, `FI_MR_CACHE_*`,
  `FI_PROVIDER`, tcp/verbs/lnx-specific vars) but **registers no `FI_CXI_*` entries** — the Cray cxi provider does
  not expose its tunables through the standard `fi_param` registration/`--env` dump; they are documented only in
  the man page (`man fi_cxi`, 1906 lines after `col -b`), summarized next.
- **`man fi_cxi`** (`fi_cxi(7)`, libfabric 2.3.1) — relevant tunables with defaults/behavior as documented (no
  explicit numeric default shown for most; "not set" unless noted):
  - `FI_CXI_DEVICE_NAME` — **"Restrict CXI provider to specific CXI devices. Format is a comma separated list of
    CXI devices (e.g. cxi0,cxi1)."** This is the one user-facing lever for picking which NIC(s) a given
    `fi_getinfo`/domain-open call can see; setting it to all 4 (or leaving unset) still yields 4 separate domains
    that the application must open and drive itself.
  - `FI_CXI_RDZV_THRESHOLD` / `FI_CXI_RDZV_GET_MIN` / `FI_CXI_RDZV_EAGER_SIZE` / `FI_CXI_RDZV_PROTO` — rendezvous
    protocol thresholds for large messages.
  - `FI_CXI_DEFAULT_CQ_SIZE`, `FI_CXI_DEFAULT_TX_SIZE`, `FI_CXI_DEFAULT_RX_SIZE` — queue sizing.
  - `FI_CXI_OPTIMIZED_MRS`, `FI_CXI_MR_MATCH_EVENTS`, `FI_CXI_PROV_KEY_CACHE` — memory-registration key behavior
    (the man page specifically recommends `FI_CXI_MR_MATCH_EVENTS=1` + `FI_CXI_OPTIMIZED_MRS=0` together for
    client/server stale-key safety).
  - `FI_CXI_ATS`, `FI_CXI_ODP`, `FI_CXI_FORCE_ODP`, `FI_CXI_ATS_MLOCK_MODE` — on-demand paging / PCIe ATS controls.
  - `FI_CXI_DISABLE_HMEM_DEV_REGISTER`, `FI_CXI_FORCE_ZE_HMEM_SUPPORT`, `FI_CXI_DISABLE_CUDA_SYNC_MEMOPS`,
    `FI_CXI_FORCE_DEV_REG_COPY` — HMEM/GPU-memory path controls.
  - `FI_CXI_HYBRID_PREEMPTIVE` / `FI_CXI_HYBRID_RECV_PREEMPTIVE` / `FI_CXI_HYBRID_POSTED_RECV_PREEMPTIVE` /
    `FI_CXI_HYBRID_UNEXPECTED_MSG_PREEMPTIVE`, `FI_CXI_RX_MATCH_MODE` — hardware-vs-software message matching
    (offload) controls, relevant to at-scale tag matching.
  - `FI_CXI_DEFAULT_VNI` — default VNI (virtual network identifier / isolation domain) if not supplied by the job
    launcher; ties into Slurm's Slingshot VNI allocation (§7).
  - No `FI_CXI_*` variable documented for "use N domains from one endpoint" or "stripe across devices" — multi-NIC
    use from one process is a **do-it-yourself** composition of multiple domains/endpoints, not a built-in
    multi-rail mode in this libfabric/cxi build.
- **Fabtests**: only `fi_info`, `fi_pingpong`, `fi_strerror` exist under `/opt/cray/libfabric/2.3.1/bin`; the full
  fabtests suite (`fi_rma_bw`, `fi_msg_bw`, `fi_dgram`, …) is **not installed** anywhere found.
- **Low-level cxi diagnostic tools** (`/usr/bin/`, part of the Cray `cxi` userspace package, not libfabric):
  `cxi_atomic_bw`, `cxi_atomic_lat`, `cxi_dump_csrs`, `cxi_gpu_loopback_bw`, `cxi_healthcheck`, `cxi_heatsink_check`,
  `cxi_read_bw`, `cxi_read_lat`, `cxi_rh`, `cxi_send_bw`, `cxi_send_lat`, `cxi_service`, `cxi_stat`, `cxi_write_bw`,
  `cxi_write_lat`. These exercise one `cxiN` device directly (`-d cxiN`-style) and would be the natural way to
  measure per-NIC and 4-NIC-concurrent bandwidth without going through SHMEM/MPI at all (not run here — only
  `cxi_stat`/`cxi_service list` were used, per the read-only/no-traffic constraint).

## 4. SHMEM

- **Cray OpenSHMEMX**: modules `cray-openshmemx/11.7.4` and `11.8.0` (default). Requires `module load cray-dsmml`
  first (prereq; conflicts with `cray-shmem`, `craype-network-none/-gemini`). Root dirs:
  `CRAY_OPENSHMEMX_ROOTDIR=/opt/cray/pe/sma/11.8.0`, `CRAY_OPENSHMEMX_DIR=/opt/cray/pe/sma/11.8.0/ofi/sma`. Headers
  at `.../include/shmem.h`, libs at `.../lib64/libsma.{a,so}`. No `oshcc`/`oshrun`; link with Cray `cc`/`CC -lsma`,
  launch via `srun`.
- **`man intro_shmem`** ("Cray OpenSHMEMX NIC Selection on the Libfabric Transport Specific Environment Variables")
  — this is the direct answer to the project's question:
  - **`SHMEM_OFI_NIC_POLICY`** — selects the **PE-to-NIC** assignment policy. **Each OpenSHMEMX PE is assigned to
    exactly one NIC** (confirms the stated constraint). Four policies, default `BLOCK`:
    - `BLOCK` — consecutive local PEs divided into contiguous blocks, one block per NIC (e.g. 22 PEs / 4 NICs →
      PEs 0-5→NIC0, 6-11→NIC1, 12-17→NIC2, 18-21→NIC3).
    - `ROUND-ROBIN` — PE i → NIC (i mod n_nics).
    - `NUMA` — PE assigned to the NIC whose NUMA node matches the PE's pinned NUMA node; **requires the PE's
      cpuset to be confined to a single NUMA node** or the job aborts with an error. Given §1.3/1.4's 1:1
      NUMA↔NIC↔GPU layout, `NUMA` policy would naturally pair each PE with its co-located NIC and GPU if PEs are
      pinned one-per-socket.
    - `USER` — explicit `SHMEM_OFI_NIC_MAPPING="nic_idx:local_pes; ..."` string (e.g.
      `"0:0-7; 2:8-31; 1:32-63"`), evaluated only when `NIC_POLICY=USER`.
  - **`SHMEM_OFI_NUM_NICS`** — caps how many NICs per node the job uses (default: "not set — OpenSHMEMX uses one
    NIC by default" per one reading, but the `NIC_POLICY` section separately says "by default, when multiple NICs
    per node are available, OpenSHMEMX attempts to use them all" — i.e. with multiple PEs/node and default
    `BLOCK` policy, all NICs get used across PEs even though each individual PE still binds only one). Format
    `"<count>:<nic_idx_list>"`, e.g. `SHMEM_OFI_NUM_NICS="2:1,3"` to use only NIC indices 1 and 3.
  - **`SHMEM_OFI_SKIP_NIC_SELECTION=1`** — bypass NIC-selection entirely, use only the first NIC presented by
    libfabric (debug mode).
  - **`SHMEM_OFI_PROVIDER_DISPLAY`** / **`SHMEM_OFI_FABRIC_DISPLAY`** — verbose NIC/provider info at `shmem_init`.
  - **`SHMEM_OFI_USE_PROV_NAME`** (default `"verbs;ofi_rxm"` — stale, that's the Slingshot-10 default; Slingshot-11
    here uses `cxi`), **`SHMEM_OFI_USE_DOMAIN_NAME`** (pick the libfabric domain explicitly, i.e. could be used to
    force a specific `cxiN`).
  - **Contexts**: `SHMEM_MAX_CTX` sets max contexts/process. For `SHMEM_THREAD_MULTIPLE` init, the default is
    "dynamically calculated maximum number of available network resources per PE in the node" — but contexts are
    **not** a mechanism to bind a PE to more than one NIC; the man page states "an independent endpoint is used per
    thread or context" **within** the NIC already assigned to that PE. `SHMEM_OFI_USE_SEP` (default 0, disabled,
    "experimental") lets multiple threads/contexts share one scalable-endpoint's connection resources on that same
    NIC — again not a path to multi-NIC-per-PE.
  - **Net answer for the project**: Cray OpenSHMEMX has no per-PE multi-NIC mode and no per-context NIC override.
    Driving all 4 NICs from one node requires either (a) 4 PEs per node (one per NIC, via `BLOCK`/`NUMA`/`USER`
    policy) — i.e. **not** "one process per node", or (b) bypassing OpenSHMEMX's transport and driving 4 raw
    libfabric cxi domains directly from one process (§3), which is outside the OpenSHMEM API.
- **Other SHMEM implementations found**:
  - **No Sandia OpenSHMEM (SOS)** installed as a module or under `/opt`/`/shared` (confirmed again; matches the
    2026-10-03 finding — `~/sos` does not exist on aac7's home either, different filesystem from aac6).
  - **rocSHMEM** *is* available: module `rocshmem/3.4.0-ro_ipc`
    (`ROCSHMEM_PATH=/shareddata/opt/rocmplus-7.14.0/rocshmem-3.4.0-ro_ipc`, built against rocm-plus 7.14.0, "step 8
    — no mpich load on OpenMPI row"). Not inspected further (no time spent reading its source/docs); rocSHMEM is
    GPU-initiated (kernel-side) SHMEM and its NIC-selection model would need separate investigation if pursued —
    worth a follow-up read of its headers/README under that path.

## 5. MPI

- **cray-mpich**: `8.1.32, 8.1.33, 9.0.0, 9.0.1, 9.1.0(default)`; also `cray-mpich-abi` (same version set) and
  `cray-mpich-ucx/{8.1.32,9.0.0}`. Default root `CRAY_MPICH_BASEDIR=/opt/cray/pe/mpich/9.1.0/ofi`.
- **`man intro_mpi`** multi-NIC section ("Section 3. Mapping processes to network interfaces") and
  `MPICH_OFI_NIC_POLICY` full entry — structurally identical to SHMEM's, **plus one extra policy**:
  - `MPICH_OFI_NIC_POLICY` = `[BLOCK | ROUND-ROBIN | NUMA | GPU | USER]`, default `BLOCK`. `BLOCK`/`ROUND-ROBIN`/
    `USER` are defined identically to the SHMEM equivalents. `NUMA` is also identical, with the added detail "if
    multiple NICs are assigned to the same NUMA node, local ranks round-robin between them."
  - **`GPU` policy (MPI-only, no SHMEM equivalent)**: "the local ranks are assigned to the NIC that is closest to
    the GPU selected by the user via a vendor API (e.g. CUDA or HIP). If multiple NICs are assigned to the same
    GPU/numa node, the local ranks will round-robin between them." This is the natural fit for "one rank per APU,
    each bound to its co-located NIC" — still **one NIC per rank**, not one process driving all 4.
  - `MPICH_OFI_NIC_MAPPING`, `MPICH_OFI_NUM_NICS` — same syntax as the SHMEM versions (`"<n>:<idx,idx,...>"`).
  - `MPICH_OFI_NIC_VERBOSE` (0/1/2) — prints NIC domain names/addresses/indices and the per-rank assignment.
  - `MPICH_GPU_SUPPORT_ENABLED=1` — required for GPU-resident buffers to work in MPI calls without an explicit
    host copy; ties into the `GTL` (GPU Transport Layer) libraries for amd_gfx906/908/etc (`PE_MPICH_GTL_DIR_*` env
    set by the module, e.g. `-L/opt/cray/pe/mpich/9.1.0/gtl/lib`).
  - `MPICH_OFI_CXI_COUNTER_REPORT[_FILE]`, `MPICH_OFI_CXI_COUNTER_VERBOSE`, `MPICH_OFI_CXI_PID_BASE` — Cassini
    hardware-counter telemetry reporting, not NIC-selection.
  - **Same fundamental limit as SHMEM**: each rank gets exactly one NIC; "all 4 NICs from one process" is not an
    MPICH policy option either. MPICH's advantage over SHMEM here is only the extra `GPU` policy convenience, not
    a different binding model.
- **Other MPI**: `openmpi/5.0.10` built multiple ways — `-libfabric2.3.1-xpmem-2.7.4-rocm-{7.0.3,7.2.0,10.0.0,
  therock-23.1.0,23.2.1}`, `-ucc1.6.0-ucx1.19.1-...` variants (same rocm matrix), plus older `5.0.8` with
  `libfabric2.2.0rc1`/`rocm-afar-*`. Both libfabric (cxi-backed) and UCX transports are available for OpenMPI; UCX
  itself has no standalone module (it's bundled into the `ucc1.6.0-ucx1.19.1` OpenMPI builds) — no separate `ucx`
  module found via `module avail`.

## 6. GPU-collective libraries

- **RCCL**: `librccl.so.1.0.70003` under `/opt/rocm-7.0.3/lib` (ROCm 7.0.3's bundled RCCL). Newer rccl builds exist
  implicitly via the `rocm-new`/`rocm-afar`/`rocm-therock` module trees (not individually enumerated).
- **rccl-tests**: modules `rccl-tests/{7.0.3, 7.2.0, 10.0.0, therock-23.1.0, therock-23.2.1, ofi-7.14.0(default)}`.
  The `ofi-7.14.0` variant's binaries live at
  `/shareddata/opt/rocmplus-ompi-7.14.0/rccl-tests-ofi-7.14.0/bin/` — `all_gather_perf, all_reduce_perf,
  all_reduce_bias_perf, alltoall_perf, alltoallv_perf, broadcast_perf, gather_perf, hypercube_perf, reduce_perf,
  reduce_scatter_perf, scatter_perf, sendrecv_perf` (standard rccl-tests set). It depends on
  `openmpi/5.0.10-ofi-7.14.0` (loaded automatically by the module) for launch, i.e. runs over libfabric/cxi through
  OpenMPI's OFI btl/MTL, not a native RCCL-OFI plugin.
- **aws-ofi-rccl / OFI RCCL network plugin**: **not found** — no `*ofi*rccl*` files under the rccl-tests install
  tree, and no separate plugin module. RCCL on this system likely falls back to its built-in net transport (socket
  or whatever RCCL auto-detects) rather than a dedicated Slingshot-aware RCCL-OFI plugin; this would need a
  deeper/slower filesystem search (`find /shareddata /opt -iname "*ofi_rccl*"` timed out under the 2-minute bound
  during this survey and was not retried with a narrower path) or a question to the cluster admins.
- **UCX**: no standalone `ucx` module; only bundled inside the OpenMPI `*-ucc1.6.0-ucx1.19.1-*` module variants
  (§5).

## 7. Launch (Slurm / PMI / Slingshot)

- `scontrol show config`: `SwitchType = switch/hpe_slingshot`, `MpiDefault = cray_shasta`,
  `SwitchParameters = vnis=1024-65535,max_acs=1018,single_node_vni=user,job_vni=user,
  tcs=DEDICATED_ACCESS:LOW_LATENCY:BULK_DATA:BEST_EFFORT`. Slurm itself allocates a **VNI** (virtual network
  isolation ID) per job from the 1024-65535 range (`job_vni=user` — job gets a VNI unless the user overrides), and
  supports 4 traffic classes (matches `FI_CXI_DEFAULT_TCLASS`'s `TC_DEDICATED_ACCESS/TC_BEST_EFFORT/...` options
  from the SHMEM man page, §4, and the `active_qos_profile` cxi_ss1 module parameter, §2).
  `LaunchParameters = use_interactive_step`. `srun --help | grep -i -A2 network` only shows the generic
  `--switches=<count>[@<max-time>]` style topology option (no Slingshot-specific `--network=` knob surfaced in the
  truncated help text — would need the full `--help` dump to confirm there isn't one elsewhere, not done here).
- `srun --mpi=list`: **`none, pmi2, pmix (pmix_v3), cray_shasta`** — i.e. the default (`cray_shasta`) is Cray's own
  PMI flavor tied to the Slingshot `switch/hpe_slingshot` plugin; `pmi2`/`pmix` are also available for
  non-Cray-MPI stacks (e.g. plain OpenMPI).
- Env inside a job (`srun --jobid=12287 ... env | grep -iE "SLINGSHOT|CXI|VNI|FI_|PMI"`): only generic
  `PMI_JOBID/PMI_LOCAL_RANK/PMI_LOCAL_SIZE/PMI_RANK/PMI_SHARED_SECRET/PMI_SIZE/PMI_UNIVERSE_SIZE` were set (single
  task, size 1) — **no `SLINGSHOT_*`, `CXI_*`, `VNI`, or `FI_*` variables are exported by default**; those would
  only appear once an actual multi-NIC-aware launch (cray-mpich/cray-openshmemx with the job_vni Slurm plugin
  active across multiple ranks) sets them, which was not exercised here (read-only constraint, job 12287 is
  occupied by someone else's run).

## 8. Benchmarks installed

| Tool | Path | Notes |
|---|---|---|
| `fi_pingpong`, `fi_info`, `fi_strerror` | `/opt/cray/libfabric/2.3.1/bin` | only 3 fabtests binaries; no `fi_rma_bw`, `fi_msg_bw`, etc. |
| `cxi_{read,write,send,atomic}_{bw,lat}`, `cxi_gpu_loopback_bw`, `cxi_healthcheck`, `cxi_heatsink_check`, `cxi_dump_csrs`, `cxi_rh`, `cxi_service`, `cxi_stat` | `/usr/bin` | Cray's own single-NIC cxi-level microbenchmarks; `cxi_gpu_loopback_bw` looks purpose-built for GPU↔NIC loopback bandwidth |
| `rccl-tests` (12 `*_perf` binaries) | `/shareddata/opt/rocmplus-ompi-7.14.0/rccl-tests-ofi-7.14.0/bin` (module `rccl-tests/ofi-7.14.0`), plus 4 more module variants (7.0.3, 7.2.0, 10.0.0, therock-\*) | standard rccl-tests set |
| OSU micro-benchmarks | not found | no `osu*` module or path located |
| IOR | not found | no `ior*` module or path located |
| Sandia OpenSHMEM (SOS) test suite | not found | SOS itself not installed |
| rocSHMEM | module `rocshmem/3.4.0-ro_ipc` only; no test binaries confirmed under `$ROCSHMEM_PATH/bin` beyond what the module prepends to `PATH` (not enumerated) | |

## 9. Summary for the "drive all 4 NICs from one process" goal

1. **Hardware supports it cleanly**: 4 independent Cassini NICs, each its own PCI domain, 1:1 with NUMA node and
   GPU, all `cxiN` device nodes world-readable/writable — nothing in the kernel driver or device permissions
   prevents one process from opening all 4.
2. **Libfabric supports it at the raw level**: `fi_info -p cxi` on a compute node returns 4 separate domains
   (cxi0-3); a single process can call `fi_domain_open`/`fi_endpoint` 4 times, once per domain, and drive all 4
   concurrently. `FI_CXI_DEVICE_NAME` can restrict/select which devices are visible to a given `fi_getinfo` call.
   This is DIY multi-rail, not a provided mode.
3. **Neither Cray OpenSHMEMX (11.8.0) nor Cray MPICH (9.1.0) exposes a multi-NIC-per-process mode.** Both have the
   *identical* `BLOCK/ROUND-ROBIN/NUMA/USER` (MPI adds `GPU`) PE-or-rank-to-NIC policy framework, and both
   explicitly assign **exactly one NIC per PE/rank**. Contexts/threads within a PE share that PE's one NIC; SEP
   (scalable endpoint) is about sharing connection resources on that same NIC, not spanning NICs.
   `SHMEM_OFI_NUM_NICS`/`MPICH_OFI_NUM_NICS` control how many NICs the **job** uses across its PEs/ranks on a node,
   not how many one PE/rank can use.
4. **Practical paths to "4 NICs, 1 process per node"**:
   - (a) Don't use OpenSHMEMX's/MPICH's transport for the multi-NIC part: open 4 raw libfabric cxi domains directly
     in-process (§3) and layer a thin custom RMA/AM path on top — most control, most engineering.
   - (b) Use 4 OpenSHMEMX PEs per node (one per NIC via `NUMA` or `USER` policy, matching the GPU/NUMA affinity in
     §1.4) instead of 1 process per node — contradicts "one process per node" but reuses the existing SHMEM API
     untouched; would need the NTT code to accept 4 PEs/node instead of 1.
   - (c) Investigate rocSHMEM (`rocshmem/3.4.0-ro_ipc`, present but unexamined) — GPU-initiated SHMEM libraries
     sometimes expose per-GPU-kernel NIC context models that differ from host-side Cray OpenSHMEMX; worth reading
     its docs/headers before ruling it out.
   - (d) Ask Cray/HPE support or check for an undocumented `FI_CXI_*`/cxi-provider multi-rail feature in a newer
     libfabric (only 2.3.1 is installed here; this was not checked against upstream libfabric's own multi-rail
     support, which may differ from Cray's build).
5. Not yet checked (would need either more time or write/traffic access beyond the read-only grant): `fi_cxi_ext.h`
   contents (possible undocumented multi-rail hooks), rocSHMEM's actual API/NIC model, whether an `aws-ofi-rccl`
   plugin exists somewhere a deeper `find` would reach, the full `srun --help` network-option list, and any
   `FI_CXI_*`/`SLINGSHOT_*` env vars that only appear under a real multi-node cray-mpich/cray-openshmemx launch.
