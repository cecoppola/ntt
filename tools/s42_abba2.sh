#!/bin/bash
# S42 abba2.sh <tag> "<ARM_A env>" "<ARM_B env>" <gate 0|1> [digits]   -- one -N2 job (s24-16 + s24-26), 1-node ABBA concurrently on each node (srun -r idx)
TAG=$1; ARM_A=$2; ARM_B=$3; GATE=${4:-0}; DPN=${5:-64410000000}
OUT=$HOME/s42/$TAG; mkdir -p $OUT/log; E=$HOME/ntt-s42/ecalc; MAXR=${MAXR:-6}; MINR=${MINR:-3}
say() { echo "$(TZ=America/New_York date +%H:%M:%S) $*" >> $OUT/summary.txt; }
rm -f $OUT/DONE
J=$(sbatch -p PPAC_MI300A_SPX -N2 -w ppac-pl1-s24-16,ppac-pl1-s24-26 --gpus=4 -t 0:45:00 -J S42 --parsable --wrap "sleep 2700")
echo $J > $OUT/jobid; say "job $J submitted"
trap 'scancel $J; echo "done $(date)" > $OUT/DONE' EXIT
for i in $(seq 1 200); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { say "job vanished"; exit 1; }; sleep 15; done
[ "$st" = RUNNING ] || { say "never ran, cancel"; exit 1; }
T0=$(date +%s); say "running on $(squeue -j $J -h -o %N)"
run() { # idx label digits outfile(or -) env...
  local idx=$1 lab=$2 d=$3 f=$4; shift 4; [ "$f" = - ] && f=""
  timeout 500 srun --jobid=$J -N1 -r $idx --gpus=4 -c 192 --overlap --exact bash -lc "module load rocm >/dev/null 2>&1; cd $E; env RNS_VERBOSE=1 ECALC_VERBOSE=2 $* ./ecalc $d $f" < /dev/null > $OUT/log/$lab.log 2>&1
}
stats() { python3 - $1 <<'P'
import sys,math
R={}
for l in open(sys.argv[1]):
    lab,arm,rc,v,tot,t=l.rstrip('\n').split('\t'); r=int(lab.split('_')[1][1:]); R.setdefault(r,{'A':[],'B':[]})[arm].append(float(tot))
d=[sum(x['B'])/2-sum(x['A'])/2 for r,x in sorted(R.items()) if len(x['A'])==2 and len(x['B'])==2]
n=len(d)
if n<2: print(n,"nan nan 0"); sys.exit()
m=sum(d)/n; sd=math.sqrt(sum((x-m)**2 for x in d)/(n-1)); se=sd/math.sqrt(n)
T={1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365}.get(n-1,2.3)
print(n,"%.2f"%m,"%.2f"%(T*se),1 if abs(m)>T*se else 0,"deltas",",".join("%.1f"%x for x in d))
P
}
node() { # idx
  local idx=$1 P=$OUT/pairs$idx.tsv; : > $P
  if [ $GATE = 1 ]; then
    local f=/dev/shm/s42_gate_$idx.txt
    run $idx gate$idx 1000000000 $f $ARM_B
    local v=$(grep -a -o 'VERIFY [A-Z]*' $OUT/log/gate$idx.log | tail -1)
    local c=$(bash $E/digcmp.sh $f $HOME/ntt/ecalc/results/e_1000000000.out); rm -f $f
    say "node$idx gate 1e9 ($ARM_B): $v, digits $c, total $(grep -a -m1 '^total' $OUT/log/gate$idx.log | awk '{print $2}')"
    [ "$v" = "VERIFY OK" ] && [ "$c" = identical ] || { say "node$idx GATE FAILED"; return 1; }
  fi
  local r=0
  while [ $r -lt $MAXR ]; do
    # a round needs ~4 runs x 100 s; do not start one with < 8 min left
    [ $(( $(date +%s) - T0 )) -gt $((2700-480)) ] && { say "node$idx no time for another round"; break; }
    r=$((r+1)); if [ $((r%2)) = 1 ]; then seq="A B B A"; else seq="B A A B"; fi; local i=0
    for arm in $seq; do
      i=$((i+1)); local lab=n${idx}_r${r}_$i$arm env=$ARM_A; [ $arm = B ] && env=$ARM_B
      run $idx $lab $DPN - $env; local rc=$?
      local v=$(grep -a -o 'VERIFY [A-Z]*' $OUT/log/$lab.log | tail -1) tot=$(grep -a -m1 '^total' $OUT/log/$lab.log | awk '{print $2}')
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $P
      say "$lab rc $rc $v total $tot"
      [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ] || { say "node$idx bad run $lab"; return 1; }
    done
    s=$(stats $P); say "node$idx after round $r (n mean(B-A) half sig): $s"; set -- $s
    [ $r -ge $MINR ] && [ "$4" = 1 ] && { say "node$idx p<0.05 stop"; break; }
  done
}
node 0 & P0=$!; node 1 & P1=$!; wait $P0; wait $P1
say "FINAL node0: $(stats $OUT/pairs0.tsv) | node1: $(stats $OUT/pairs1.tsv)"
