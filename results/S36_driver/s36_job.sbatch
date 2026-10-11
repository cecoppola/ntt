#!/bin/bash
# S36: the 4-node job that runs s36_batch.sh directly (no sleep hold): the allocation ends when the driver exits.  Submitted with
#   --dependency=afterany:12452 (the QOS allows 6 nodes per user; S35's 4 are held until it ends).  Copied to ~/s36.
#SBATCH -p 192C4G1H_MI300A_RHEL9_A1
#SBATCH -N 4
#SBATCH --exclusive
#SBATCH -c 192
#SBATCH --gpus-per-node=4
#SBATCH -t 5:00:00
#SBATCH -J S36
#SBATCH -x aac7-n01,aac7-n61
#SBATCH -o ~/s36/job.out
exec bash ~/s36/s36_batch.sh
