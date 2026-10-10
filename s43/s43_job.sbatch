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
#SBATCH -x aac7-n01,aac7-n61
#SBATCH -o ~/s43/job.out
exec bash ~/s43/s43_batch.sh
