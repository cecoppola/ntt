#!/bin/bash
# mnrun.sh - run <procs> node-processes of ecalc (or any command) connected by the TCP inter-node
# communicator (Phase 8 M1).  Correctness only.  The processes are spread over the nodes of the
# allocation, several per node when there are fewer nodes (they share the node's four APUs).
#   mnrun.sh <procs> <command...>          e.g.  SLURM_JOB_ID=<id> ./mnrun.sh 2 ./ecalc 1000000000 /tmp/e.txt
set -e
P=$1; shift
[ -n "$SLURM_JOB_ID" ] || { echo "set SLURM_JOB_ID to the allocation"; exit 1; }
nodes=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)")
nn=$(echo "$nodes" | wc -l); per=$(( (P + nn - 1) / nn ))
hosts=$(echo "$nodes" | awk -v per=$per -v P=$P 'BEGIN { n = 0 } { i = 0; while (i < per && n < P) { printf "%s%s", n ? "," : "", $1; i++; n++ } }')
export COMM_HOSTS="$hosts" COMM_PORT=${COMM_PORT:-$((20000 + RANDOM % 30000))}   # a per-run port base: a straggler of a failed run must not catch the next run's connections (M3 uses base .. base + 6656)
exec srun --jobid="$SLURM_JOB_ID" -N "$nn" --ntasks="$P" --ntasks-per-node="$per" --gpus-per-node=4 --overlap --export=ALL \
     bash -lc 'module load rocm; export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec "$@"' _ "$@"
