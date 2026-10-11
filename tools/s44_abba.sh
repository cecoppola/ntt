#!/bin/bash
# s44_abba.sh (S44, aac6 SH5_MI300A_CPX node): ABBA of two env settings at DIGITS digits (POOL_LOG=29, 3 XCD devices of one APU), no digit output.
#   usage: s44_abba.sh <jobid> <outdir> "<armA env>" "<armB env>" [digits]    env: WT (clone), MINR (3), MAXR (6)
# Round = 4 runs, A B B A (odd) / B A A B (even); delta = mean(B)-mean(A) per round; after each round >= MINR stop when the paired t-test on the
# round deltas gives p < 0.05, or at MAXR.  Table <outdir>/pairs.tsv, verdict <outdir>/RESULT.  Kills only its own srun via timeout; never scancel.
J=$1; OUT=$2; A=$3; B=$4; DIG=${5:-4000000000}
WT=${WT:-$HOME/ntt-s44}; MINR=${MINR:-3}; MAXR=${MAXR:-6}
mkdir -p $OUT/log; : > $OUT/pairs.tsv
one() { # arm label
  local arm=$1 lab=$2 cw="$A"; [ $arm = B ] && cw="$B"
  timeout 400 srun --jobid=$J --overlap bash -lc "module load rocm; cd $WT/ecalc; env POOL_LOG=29 $cw ./ecalc $DIG" < /dev/null > $OUT/log/$lab.log 2>&1; local rc=$?
  local v tot; v=$(grep -a -m1 '^VERIFY' $OUT/log/$lab.log); tot=$(grep -a -m1 '^total' $OUT/log/$lab.log | awk '{print $2}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%H:%M:%S)" >> $OUT/pairs.tsv
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ]
}
stats() { python3 - $OUT/pairs.tsv <<'P'
import sys,math
R={}
for l in open(sys.argv[1]):
    lab,arm,rc,v,tot,t=l.rstrip('\n').split('\t'); R.setdefault(int(lab.split('_')[0][1:]),{'A':[],'B':[]})[arm].append(float(tot))
d=[sum(x['B'])/2-sum(x['A'])/2 for r,x in sorted(R.items()) if len(x['A'])==2 and len(x['B'])==2]
n=len(d)
if n<2: print(n,"nan nan 0"); sys.exit()
m=sum(d)/n; se=math.sqrt(sum((x-m)**2 for x in d)/(n-1))/math.sqrt(n)
T=[0,12.706,4.303,3.182,2.776,2.571,2.447,2.365][n-1]; h=T*se
print(n,"%.3f"%m,"%.3f"%h,1 if abs(m)>h else 0,"deltas",",".join("%.2f"%x for x in d))
P
}
r=0; why=MAXR
while [ $r -lt $MAXR ]; do
  r=$((r+1)); [ $((r%2)) = 1 ] && seq="A B B A" || seq="B A A B"; i=0
  for arm in $seq; do i=$((i+1)); one $arm r${r}_$i$arm || { echo "FAILED: bad run r${r}_$i$arm" > $OUT/RESULT; exit 1; }; done
  s=$(stats); set -- $s
  if [ $r -ge $MINR ] && [ "$4" = 1 ]; then why="p<0.05"; break; fi
done
echo "DONE rounds=$r stop=$why: n mean(B-A)[s] halfCI95[s] sig deltas: $(stats)" > $OUT/RESULT
