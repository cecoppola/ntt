#!/bin/bash
# a14_drive.sh: unattended launch-to-launch probe loop on a 2-node aac7 hold.  usage:
#   HOLD=<job> NN=2 ROUNDS=<r> CONFIGS="base|;tcA|SHMEM_OFI_DEFAULT_TCLASS=TC_BULK_DATA;..." [OUT=~/sess1011c/a14/R] [PMAX=64 PREPS=100] bash a14_drive.sh
# Each round runs every config once (order shuffled); one launch = mnrun.sh $NN env COMM_TRANSPORT=shmem <config env> t_comm --bw 4 PREPS PMAX.
# Output OUT/launches.tsv: round label cfg et rate64 (mean over PEs, GB/s per thread) rate_max wall grpA grpB (MCU-counter deltas summed over nodes/NICs).  Stops at DEADLINE epoch, STOP file, or ROUNDS.
: ${HOLD:?}; NN=${NN:-2}; OUT=${OUT:-$HOME/sess1011c/a14/R}; PMAX=${PMAX:-64}; PREPS=${PREPS:-100}; ROUNDS=${ROUNDS:-10}; DEADLINE=${DEADLINE:-0}
WT=${WT:-$HOME/ntt-wt/a14}; mkdir -p $OUT/log; rm -f $OUT/DONE; export SNAPDIR=$OUT/nic
cd $WT/ecalc && source aac7env.sh >/dev/null 2>&1; export SLURM_JOB_ID=$HOLD
W=$HOME/sess1011c/a14/wrap.sh
[ -s $OUT/launches.tsv ] || printf 'round\tlabel\tcfg\tet\trate64\trate_max\twall\tgrpA\tgrpB\tfgfc\n' > $OUT/launches.tsv
IFS=';' read -ra CF <<< "$CONFIGS"
grp() { # label -> "A B fgfc" (delta over nodes+NICs)
  for h in $(ls $SNAPDIR/$1.pre.* | sed "s/.*pre\.//"); do
    awk -v pre=$SNAPDIR/$1.pre.$h -v post=$SNAPDIR/$1.post.$h 'BEGIN{while((getline l<pre)>0){split(l,a," ");p[a[1]]=a[2]} while((getline l<post)>0){split(l,a," ");d=a[2]-p[a[1]]; if(a[1]~/oxe_mcu_meas_(1[89]|2[0-5])$/)A+=d; else if(a[1]~/oxe_mcu_meas_(2[89]|3[0-3])$/)B+=d; else if(a[1]~/fgfc/)F+=d} printf "%d %d %d\n",A,B,F}'
  done | awk '{A+=$1;B+=$2;F+=$3} END{printf "%d\t%d\t%d",A,B,F}'; }
for r in $(seq 1 $ROUNDS); do
  for i in $(shuf -i 0-$((${#CF[@]}-1))); do
    [ -e $OUT/STOP ] && break 2; [ $DEADLINE -gt 0 ] && [ $(date +%s) -ge $DEADLINE ] && break 2
    c=${CF[$i]}; name=${c%%|*}; envw=${c#*|}; lab=r$(printf %02d $r)_$name; log=$OUT/log/$lab.log; t0=$(date +%s)
    ( exec env MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_SHMEM_POOL_MB=6000 COMM_PORT=$((24000 + RANDOM % 900)) timeout 150 ./mnrun.sh $NN env COMM_TRANSPORT=shmem FI_MR_CACHE_MONITOR=userfaultfd $envw bash $W $lab ./tests/t_comm --bw 4 $PREPS $PMAX ) > $log 2>&1 < /dev/null
    wall=$(( $(date +%s) - t0 )); pl=$log.plain; sed -E 's/^ *[0-9]+: //' $log > $pl
    r64=$(grep -a "t_comm bw: pe [0-9]* (rank" $pl | grep -a " $((64*1048576)) B per slab" | sed -nE 's/.*, ([0-9.]+) GB\/s per thread.*/\1/p' | awk '{v+=$1;n++} END{if(n>0) printf "%.2f", v/n; else print "NA"}')
    rmx=$(grep -a "t_comm bw: pe [0-9]* (rank" $pl | grep -a " $((PMAX*1048576)) B per slab" | sed -nE 's/.*, ([0-9.]+) GB\/s per thread.*/\1/p' | awk '{v+=$1;n++} END{if(n>0) printf "%.2f", v/n; else print "NA"}')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $r $lab "$name" $(TZ=America/New_York date +%H:%M:%S) $r64 $rmx $wall "$(grp $lab)" >> $OUT/launches.tsv
    tail -1 $OUT/launches.tsv
  done
done
date > $OUT/DONE; [ "${KEEP_HOLD:-0}" = 1 ] || { [ "$(squeue -j $HOLD -h -o '%u %j')" = "<user> a14" ] && scancel $HOLD; }
