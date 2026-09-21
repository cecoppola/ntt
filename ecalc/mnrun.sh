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
    export COMM_SHMEM_POOL_MB=$POOL SHMEM_SYMMETRIC_HEAP_SIZE=${SHMEM_SYMMETRIC_HEAP_SIZE:-$((POOL + 512))M}
    export OMPI_MCA_memheap_base_max_segments=${OMPI_MCA_memheap_base_max_segments:-64}
    exec srun --jobid="$SLURM_JOB_ID" --mpi=pmix -N "$nn" -w "$list" --ntasks="$P" --ntasks-per-node="$per" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
         bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec setarch x86_64 -L "$@"' _ "$@"
fi
exec srun --jobid="$SLURM_JOB_ID" -N "$nn" -w "$list" --ntasks="$P" --ntasks-per-node="$per" --distribution=block --gpus-per-node=4 --overlap --export=ALL \
     bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec "$@"' _ "$@"
