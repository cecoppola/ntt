#!/bin/bash
# aac7q queue item (O2 follow-up, TASKS 36, results/C03.md, results/O2.md): re-time C4 COMM_SHMEM_RPIPE=1 and C5 MN_REDIST_FUSE=1
# with the gather-stall fix on (DBIG_SMALL_BOUNCE=1 in every arm).  Installed by 70_o2_bounce.sh only when O2 was clean.
# 4 nodes x 6.441e10, aac7 line + COMM_XSTATS=1 MN_WAIT_STATS=1, no digit writes; rounds of three runs in rotating order
# (A C4 C5 / C5 A C4 / C4 C5 A), round cap 3, no run starts after 38 min.  Paired B-A per round for C4 and C5 (tools/o2_stats.py).
# Output ~/o2/c45/: log/ (+ c4/ c5/ link dirs), summary.txt, stats_c4.txt, stats_c5.txt; done-marker ~/o2/c45/C45_DONE.
# OWNER=o2
# NODES=4
# EST_MIN=45
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
# ALLOW_SLOW=0
set -u
WT=$HOME/ntt-wt/o2; E=$WT/ecalc; O=$HOME/o2/c45; NN=4; DPN=64410000000
mkdir -p $O/log $O/c4 $O/c5; cd $E || exit 2
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh)
case "$LINE" in *COMM_SHMEM_ROUND_MB=*MN_T_CHUNK_MB=2048*) ;; *MN_T_CHUNK_MB=2048*COMM_SHMEM_ROUND_MB=*) ;; *) echo "unexpected LINE: $LINE"; exit 2;; esac
EC=$HOME/o2/ecalc_o2
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok"; bash -n $0 && echo syntax ok; strings $EC | grep -q DBIG_SMALL_BOUNCE && strings $EC | grep -q MN_REDIST_FUSE && echo binary ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s); S=$O/summary.txt; : > $S
el() { echo $(( $(date +%s) - T0 )); }
source aac7env.sh >/dev/null 2>&1
ldd $EC | grep -q libsma || { echo "binary without SHMEM" >> $S; touch $O/C45_DONE; exit 2; }
echo "start $(date) hold $J hosts $(squeue -j $J -h -o %N) binary $(md5sum $EC | cut -c1-12)" >> $S
X="ECALC_SEGV_TRACE=1 FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1 DBIG_SMALL_BOUNCE=1"
one() { # round arm
  local k=$1 arm=$2 aw=""; case $arm in C4) aw=COMM_SHMEM_RPIPE=1;; C5) aw=MN_REDIST_FUSE=1;; esac
  [ $(el) -lt 2280 ] || { echo "skip r${k}_$arm (time)" >> $S; return 1; }
  srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 -c1 timeout 100 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' < /dev/null > /dev/null 2>&1
  local lab=c45_r${k}_$arm t1=$(date +%s)
  SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 420 ./mnrun.sh $NN env $LINE $X $aw $EC $((NN * DPN)) < /dev/null > $O/log/$lab.log 2>&1; local rc=$?
  echo "$lab rc=$rc wall=$(( $(date +%s) - t1 )) el=$(el) $(grep -a -c 'all 4 nodes: VERIFY OK' $O/log/$lab.log)verify $(grep -a -m1 ': total ' $O/log/$lab.log | cut -c1-70)" >> $S
  case $arm in A) ln -sf ../log/$lab.log $O/c4/x_p${k}_A.log; ln -sf ../log/$lab.log $O/c5/x_p${k}_A.log;;
               C4) ln -sf ../log/$lab.log $O/c4/x_p${k}_B.log;; C5) ln -sf ../log/$lab.log $O/c5/x_p${k}_B.log;; esac
  return 0; }
for k in 1 2 3; do
  case $k in 1) ord="A C4 C5";; 2) ord="C5 A C4";; 3) ord="C4 C5 A";; esac
  for a in $ord; do one $k $a || break 2; done
done
python3 $WT/tools/o2_stats.py $O/c4 > $O/stats_c4.txt 2>&1; python3 $WT/tools/o2_stats.py $O/c5 > $O/stats_c5.txt 2>&1
{ echo "C4 (B = COMM_SHMEM_RPIPE=1):"; grep '^paired' $O/stats_c4.txt; echo "C5 (B = MN_REDIST_FUSE=1):"; grep '^paired' $O/stats_c5.txt; } >> $S
squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 >> $S
echo "DONE $(date) el=$(el)" >> $S; touch $O/C45_DONE; exit 0
