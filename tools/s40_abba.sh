#!/bin/bash
# s40_abba.sh (S40, from s38_abba.sh) - 1-node ABBA of DBIG_MAXIDX_TOP (A = ARM_A, B = ARM_B) at 6.441e10 digits on a Slurm hold's node, no digit writes.
#   usage: s40_abba.sh <hold> <outdir> [digits]       env: WT (default ~/ntt-wt/s38), MAXR (8), MINR (3), HALF_S (3.0: CI half-width stop), ARM_A, ARM_B (env of each arm; here DBIG_ADDSUB2=1 and DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1)
# A round = 4 runs, A B B A (odd rounds) or B A A B (even), the per-round delta = mean(B) - mean(A).  After each round >= MINR: stop when the paired t-test
# on the round deltas gives p < 0.05, or the 95 % CI half-width <= HALF_S, or MAXR rounds.  Marker <outdir>/S40_DONE (SUCCESS / FAILED), table <outdir>/pairs.tsv.
# Never cancels or modifies a job; kills only its own srun PID (rd_run).
HOLD=$1; OUT=$2; DPN=${3:-64410000000}
WT=${WT:-$HOME/ntt-wt/s40}; E=$WT/ecalc; MAXR=${MAXR:-8}; MINR=${MINR:-3}; HALF_S=${HALF_S:-3.0}; ARM_A=${ARM_A:-DBIG_ADDSUB2=1}; ARM_B=${ARM_B:-DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1}
source $WT/tools/rundriver.sh; rd_init $OUT S40_DONE
cd $E || exit 2; source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { RD_VERDICT="FAILED: no MNRUN_MODULES"; exit 2; }
export SLURM_JOB_ID=$HOLD
NODE=$(squeue -j $HOLD -h -o %N); [ -n "$NODE" ] || { RD_VERDICT="FAILED: hold $HOLD has no node"; exit 2; }
: > $OUT/pairs.tsv
one() {   # arm label
  local arm=$1 lab=$2 cw="$ARM_A"; [ $arm = B ] && cw="$ARM_B"
  rd_health $HOLD $NODE >/dev/null 2>&1 || { sleep 60; rd_health $HOLD $NODE >/dev/null 2>&1; } || { rd_say "unhealthy $NODE"; return 9; }
  rd_run $lab 450 srun --jobid=$HOLD -N1 -w $NODE --gpus=4 -c 192 --overlap bash -lc "module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $E; env ECALC_SEGV_TRACE=1 $cw ./ecalc $DPN" < /dev/null; local rc=$?
  local P=$OUT/log/$lab.plain; cp $OUT/log/$lab.log $P
  local v tot; v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | awk '{print $2}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/pairs.tsv
  rd_say "$lab arm $arm rc $rc $v total $tot"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ] || { rd_say "bad run $lab"; return 1; }
}
stats() {   # prints "n mean_delta half p_lt_05" over complete rounds
  python3 - $OUT/pairs.tsv <<'P'
import sys,math
rows=[l.rstrip('\n').split('\t') for l in open(sys.argv[1])]
R={}
for lab,arm,rc,v,tot,t in rows:
    r=int(lab.split('_')[0][1:]); R.setdefault(r,{'A':[],'B':[]})[arm].append(float(tot))
d=[sum(x['B'])/2-sum(x['A'])/2 for r,x in sorted(R.items()) if len(x['A'])==2 and len(x['B'])==2]
n=len(d)
if n<2: print(n,"nan nan 0"); sys.exit()
m=sum(d)/n; sd=math.sqrt(sum((x-m)**2 for x in d)/(n-1)); se=sd/math.sqrt(n)
T={1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262}[n-1] if n-1 in range(1,10) else 2.2
half=T*se
print(n,"%.2f"%m,"%.2f"%half,1 if abs(m)>half else 0, "deltas",",".join("%.1f"%x for x in d))
P
}
r=0; why="MAXR"
while [ $r -lt $MAXR ]; do
  r=$((r+1)); if [ $((r % 2)) = 1 ]; then seq="A B B A"; else seq="B A A B"; fi; i=0; bad=0
  for arm in $seq; do i=$((i+1)); one $arm r${r}_${i}$arm || { bad=1; break; }; done
  [ $bad = 1 ] && { RD_VERDICT="FAILED: bad run in round $r (see $OUT/summary.txt, $OUT/ALERT)"; exit 1; }
  s=$(stats); rd_say "after round $r: n mean(B-A) half-width(95%) significant: $s"
  set -- $s; n=$1; half=$3; sig=$4
  if [ $r -ge $MINR ]; then
    [ "$sig" = 1 ] && { why="p<0.05"; break; }
    awk -v h="$half" -v t="$HALF_S" 'BEGIN{exit !(h+0<=t+0)}' && { why="CI half-width <= $HALF_S s"; break; }
  fi
done
RD_VERDICT="SUCCESS: S40 ABBA done after $r rounds ($why): $(stats)"
