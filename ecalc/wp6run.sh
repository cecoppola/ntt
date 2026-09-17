#!/bin/bash
# wp6run.sh - run one process per APU across the nodes of the current Slurm
# allocation, connected by the TCP communicator (WP6, correctness only).
#   wp6run.sh <nodes> <command...>
# e.g. salloc -p PPAC_MI300A_SPX -N2 --gpus-per-node=4 -t 1:00:00 --no-shell
#      SLURM_JOB_ID=<id> ./wp6run.sh 2 ./tests/t_dist 24
# COMM_HOSTS lists the node of every rank (4 ranks per node); rank r uses APU r mod 4.
set -e
N=$1; shift
[ -n "$SLURM_JOB_ID" ] || { echo "set SLURM_JOB_ID to the allocation"; exit 1; }
hosts=$(scontrol show hostnames "$(squeue -j "$SLURM_JOB_ID" -h -o %N)" | head -n "$N" | awk '{for (i = 0; i < 4; i++) printf "%s%s", (NR > 1 || i > 0) ? "," : "", $1}')
export COMM_HOSTS="$hosts" COMM_PORT=${COMM_PORT:-27000}
exec srun --jobid="$SLURM_JOB_ID" -N "$N" --ntasks-per-node=4 --gpus-per-node=4 --export=ALL \
     bash -c 'export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec "$@"' _ "$@"
