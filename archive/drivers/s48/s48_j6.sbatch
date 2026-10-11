#!/bin/bash
# S48 J6: the 6-node scaling points (2 runs at 6.441e10 digits/node).  Submitted in advance (BATCH_SLOTS J6); exits itself, marker ~/s48/done_n6, J6_DONE.
#SBATCH -p 192C4G1H_MI300A_RHEL9_A1
#SBATCH -N 6
#SBATCH --exclusive
#SBATCH -c 192
#SBATCH --gpus-per-node=4
#SBATCH -t 60
#SBATCH -J b1011w
#SBATCH -x aac7-n01,aac7-n61
#SBATCH -o ~/s48/j6.out
rm -f $HOME/s48/J6_DONE
echo "J6 start $(date +%s) $(TZ=America/New_York date) job $SLURM_JOB_ID nodes $SLURM_JOB_NODELIST" > $HOME/s48/J6_START
bash $HOME/s48/s48_run.sh 6 2
rc=$?
echo "rc=$rc $(cat $HOME/s48/done_n6 2>/dev/null) end $(TZ=America/New_York date)" > $HOME/s48/J6_DONE
exit $rc
