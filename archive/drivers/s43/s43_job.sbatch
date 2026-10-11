#!/bin/bash
# S43: the 4-node job that runs s43_batch.sh directly (the allocation ends when the driver exits).
#   Submitted with --dependency=afterany:12461 (S36).  Copied to ~/s43.
#SBATCH -p 192C4G1H_MI300A_RHEL9_A1
#SBATCH -N 4
#SBATCH --exclusive
#SBATCH -c 192
#SBATCH --gpus-per-node=4
#SBATCH -t 3:00:00
#SBATCH -J S43
#SBATCH -x x9000c1s0b1n0,x9000c1s6b1n0
#SBATCH -o /shared/midgard/home/chcoppola/s43/job.out
exec bash /shared/midgard/home/chcoppola/s43/s43_batch.sh
