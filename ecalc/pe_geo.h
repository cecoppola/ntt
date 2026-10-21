/* pe_geo.h -- per-PE topology: GPU, NICs, NUMA node, CPU list (xshmem stage S0; built with XSHMEM=ofi).
 * Nothing is hard-coded: PEs per node, GPUs, NICs per NUMA node all come from the launch and sysfs.
 * Chain: device -> PCI bus id -> sysfs numa_node -> cxi devices on that node -> /sys/devices/system/node/N/cpulist.
 * Env overrides: COMM_OFI_NICS ("0;1;2;3" or "0,4;1,5;2,6;3,7", `;` between node-local GPUs), XSHMEM_LANES,
 * XSHMEM_PES_PER_GPU, PE_GEO_SYSFS_ROOT (prefix of every /sys path: mock trees for tests). */
#ifndef PE_GEO_H
#define PE_GEO_H
#define PE_GEO_MAXNIC 16
#define PE_GEO_CPUS 512
typedef struct {
    int pe, npes;             /* rank in the job, size */
    int node, nnodes;         /* node = pe / pes_per_node (block launch) */
    int local, pes_per_node;  /* index on the node (SLURM_LOCALID), PEs on each node */
    int pes_per_gpu;          /* XSHMEM_PES_PER_GPU, default 1 */
    int gpu;                  /* HIP device ordinal to use (0 if one device is visible); -1 if no GPU is involved */
    int gpu_node;             /* node-local GPU index = local / pes_per_gpu (what COMM_OFI_NICS is indexed by) */
    int nic[PE_GEO_MAXNIC];   /* cxi device numbers of this PE's NICs */
    int nics;                 /* 0 = NIC-less (loopback/host) */
    int nlanes;               /* XSHMEM_LANES if set (>=1, may exceed nics: virtual lanes), else 1 */
    int numa;                 /* NUMA node of the GPU */
    char cpus[PE_GEO_CPUS];   /* CPU list string of that NUMA node ("" if unknown) */
} pe_topo;

/* pure part, no GPU/boot: busid = lowercase "0000:05:00.0" of the GPU or NULL (then numa = gpu_node % number of NUMA nodes).
 * ngpu = visible devices (>=1 selects gpu = gpu_node % ngpu, or 0 if ngpu == 1; 0 = no GPU).
 * Returns 0, or -1 with the message in pe_geo_err(). */
int pe_geo_from(pe_topo *t, int pe, int npes, int local, int ppn, int ngpu, const char *busid);
const char *pe_geo_err(void);
/* one-line description, e.g. "pe 5/8 node 1/2 local 1/4 gpu 1 numa 1 nics 2 [1,5] lanes 1 cpus 48-95" */
void pe_geo_str(const pe_topo *t, char *buf, int n);

#ifndef PE_GEO_NOBOOT
/* full discovery inside a launched job: boot_init (rank/size), SLURM_LOCALID, ppn from SLURM_NTASKS_PER_NODE /
 * SLURM_STEP_TASKS_PER_NODE (single number) or else counted from a node-identity allgather; GPU count and bus id from HIP
 * under USE_HIP. Aborts on failure. */
void pe_geo_init(pe_topo *t);
#endif
#endif
