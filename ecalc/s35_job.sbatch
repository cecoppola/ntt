#!/bin/bash
# S35: the 4-node job that runs s35_batch.sh directly (no sleep hold): the allocation ends when the driver exits.  Copied to ~/s35.
#SBATCH -p 192C4G1H_MI300A_RHEL9_A1
#SBATCH -N 4
#SBATCH --exclusive
#SBATCH -c 192
#SBATCH --gpus-per-node=4
#SBATCH -t 14:00:00
#SBATCH -J S35
#SBATCH -x x9000c1s6b1n0,x9000c1s0b1n0
#SBATCH -o /shared/midgard/home/chcoppola/s35/job.out
exec bash /shared/midgard/home/chcoppola/s35/s35_batch.sh
