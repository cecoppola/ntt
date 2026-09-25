#!/bin/bash
# mnrun.sh - run <procs> node-processes of ecalc (or any command) connected by the inter-node
# communicator (Phase 8 M1: TCP; Phase 11 S: SHMEM with COMM_TRANSPORT=shmem).  Correctness only on
# aac6.  The processes are spread over the nodes of the allocation, several per node when there are
# fewer nodes (they share the node's four APUs); the node count used is the largest that divides
# <procs> (Slurm places tasks by blocks otherwise, and COMM_HOSTS must match the placement), listed explicitly.
#   mnrun.sh <procs> <command...>          e.g.  SLURM_JOB_ID=<id> ./mnrun.sh 2 ./ecalc 1000000000 /tmp/e.txt
# SHMEM (COMM_TRANSPORT=shmem): the same placement, launched by srun's PMIx (OpenMPI 4.1.6 OSHMEM on aac6; the
# target's srun likewise), rank = PE.  COMM_SHMEM_POOL_MB (8192) sizes the transport's symmetric pool; the OSHMEM
# heap is set 512 MiB above it.  setarch -L (the legacy bottom-up mmap layout) makes OSHMEM's scan of the static
# data segments deterministic across the PEs -- without it every anonymous mapping below liboshmem (thread stacks,
# malloc arenas) is registered and their count differs per PE, and shmem_init crashes in the key exchange about
# half the time (results/S.md).
set -e
P=$1; shift
[ -n "$SLURM_JOB_ID" ] || { echo "set SLURM_JOB_ID to the allocation"; exit 1; }
nodes=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)")
nn=$(echo "$nodes" | wc -l); [ "$nn" -gt "$P" ] && nn=$P
while [ $((P % nn)) -ne 0 ]; do nn=$((nn - 1)); done
per=$((P / nn))
use=$(echo "$nodes" | head -n "$nn")
hosts=$(echo "$use" | awk -v per=$per '{ for (i = 0; i < per; i++) printf "%s%s", (NR > 1 || i) ? "," : "", $1 }')
list=$(echo "$use" | paste -sd,)
export COMM_HOSTS="$hosts" COMM_PORT=${COMM_PORT:-$((20000 + RANDOM % 6000))}    # a per-run port base: a straggler of a failed run must not catch the next run's connections (M3 uses base .. base + 6656); below the ephemeral range 32768-60999, where a listener collides with any outgoing connection now and then (S: "bind: Address already in use" once in ~4 runs at 8 processes)
if [ "$COMM_TRANSPORT" = shmem ]; then
    POOL=${COMM_SHMEM_POOL_MB:-8192}
    export COMM_SHMEM_POOL_MB=$POOL
    # Phase 12 S: the implementation the binary was built against: SOS (make SHMEM_HOME=~/sos; libsma) or OSHMEM (oshcc).
    # COMM_SHMEM_IMPL=sos|oshmem overrides the detection.
    # Phase 14 N4 (A4): the detection looks through wrappers (env, stdbuf, timeout, numactl, setarch, nice, ...): every word of
    # the command that resolves to an executable ELF file is checked for libsma / liboshmem in its dynamic section, the
    # first that links either decides (the wrappers link neither); none found: oshmem, as before, with a note.
    impl=${COMM_SHMEM_IMPL:-}
    case "$impl" in ""|sos|oshmem) ;; *) echo "mnrun.sh: COMM_SHMEM_IMPL=$impl: use sos or oshmem"; exit 1;; esac
    if [ -z "$impl" ]; then
        for a in "$@"; do
            case "$a" in *=*|-*) continue;; esac                                 # env assignments, wrapper options
            f=$(command -v -- "$a" 2>/dev/null) || continue; [ -f "$f" ] && [ -x "$f" ] || continue
            head -c 4 "$f" 2>/dev/null | grep -q ELF || continue                  # scripts: their interpreter is not the binary
            need=$(readelf -d "$f" 2>/dev/null | grep NEEDED; ldd "$f" 2>/dev/null)
            if echo "$need" | grep -q libsma; then impl=sos; break; fi
            if echo "$need" | grep -q liboshmem; then impl=oshmem; break; fi
        done
        [ -n "$impl" ] || { impl=oshmem; echo "mnrun.sh: no libsma/liboshmem found in the command's executables; assuming oshmem (set COMM_SHMEM_IMPL)" >&2; }
    fi
    [ -n "${MNRUN_SHOW_IMPL:-}" ] && { echo "mnrun.sh: SHMEM implementation $impl"; [ "$MNRUN_SHOW_IMPL" = only ] && exit 0; }   # (test hook: MNRUN_SHOW_IMPL=only prints and stops)
    if [ "$impl" = sos ]; then
        # SOS: PMI-1 (simple PMI) under srun's pmi2 plugin; the sockets provider of libfabric (the tcp provider lacks what SOS asks
        # for); the pool is the transport's own shmem_malloc (SHMEM_SYMMETRIC_SIZE) or, with COMM_SHMEM_DEVHEAP=1, a HIP buffer
        # registered as SOS's external heap (results/S12.md).  SOS's internal heap is only its own bookkeeping then.
        if [ "${COMM_SHMEM_DEVHEAP:-0}" != 0 ]; then export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-64M}; else export SHMEM_SYMMETRIC_SIZE=${SHMEM_SYMMETRIC_SIZE:-$((POOL + 512))M}; fi
        export FI_PROVIDER=${FI_PROVIDER:-sockets} SHMEM_OFI_PROVIDER=${SHMEM_OFI_PROVIDER:-sockets} SHMEM_DISABLE_ASLR_CHECK=1
        exec srun --jobid="$SLURM_JOB_ID" --mpi=pmi2 -N "$nn" -w "$list" --ntasks="$P" --ntasks-per-node="$per" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
             bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec "$@"' _ "$@"
    fi
    export SHMEM_SYMMETRIC_HEAP_SIZE=${SHMEM_SYMMETRIC_HEAP_SIZE:-$((POOL + 512))M}
    export OMPI_MCA_memheap_base_max_segments=${OMPI_MCA_memheap_base_max_segments:-64}
    exec srun --jobid="$SLURM_JOB_ID" --mpi=pmix -N "$nn" -w "$list" --ntasks="$P" --ntasks-per-node="$per" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
         bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec setarch x86_64 -L "$@"' _ "$@"
fi
exec srun --jobid="$SLURM_JOB_ID" -N "$nn" -w "$list" --ntasks="$P" --ntasks-per-node="$per" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
     bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec "$@"' _ "$@"
