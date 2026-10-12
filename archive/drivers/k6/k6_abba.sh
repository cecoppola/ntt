#!/bin/bash
# k6_abba.sh (K6, from s51_abba.sh) - aac6 paired ABBA of RNS_DIST_OVERLAP (A = default, B = RNS_DIST_OVERLAP=1) inside a Slurm hold, no digit writes.
#   usage: k6_abba.sh <hold> <outdir> [digits]      (DRY=1: the logic with stub runs, no Slurm)
#   env: WT (~/ntt-k6), MAXR (8), MINR (3), HALF_S (1.0 s CI stop), MARK (K6_DONE), ARM_A (""), ARM_B (RNS_DIST_OVERLAP=1), STOP_EPOCH (start no run
#        that cannot end by it), RUN_S (expected wall of one run), XE (words for both arms)
# A round = A B B A (odd) / B A A B (even); per-round delta = mean(B) - mean(A) of ecalc's `total`, and of the C-tier seconds (the
# "rns_dist: RNS_STRATEGY=auto ... C <n> products <s> s" line).  After >= MINR rounds: stop at p < 0.05 on the total, CI half-width <= HALF_S, or MAXR.
# Resumable: rounds already in <outdir>/pairs.tsv count.  Marker <outdir>/<MARK> (SUCCESS / FAILED / deadline), tables pairs.tsv, ctier.tsv, stats.txt.
# Kills only rd_run's own srun PID; never scancel.
HOLD=$1; OUT=$2; DPN=${3:-64410000000}; DRY=${DRY:-0}
WT=${WT:-$HOME/ntt-k6}; E=$WT/ecalc; MAXR=${MAXR:-8}; MINR=${MINR:-3}; HALF_S=${HALF_S:-1.0}; MARK=${MARK:-K6_DONE}
ARM_A=${ARM_A:-}; ARM_B=${ARM_B:-RNS_DIST_OVERLAP=1}; STOP_EPOCH=${STOP_EPOCH:-0}; RUN_S=${RUN_S:-140}; XE=${XE:-}
source $WT/tools/rundriver.sh; rd_init $OUT $MARK
cd $E || exit 2
if [ "$DRY" != 1 ]; then
  NODES=$(squeue -j $HOLD -h -o %N); export SLURM_JOB_ID=$HOLD
  [ -x $E/ecalc ] && grep -aqF RNS_DIST_OVERLAP $E/ecalc || { RD_VERDICT="FAILED: $E/ecalc missing or without RNS_DIST_OVERLAP"; exit 2; }
else NODES="n1"; fi
rd_say "K6 ABBA: node $NODES hold $HOLD digits $DPN ; A = '$XE $ARM_A' ; B = '$XE $ARM_B' ; stop epoch $STOP_EPOCH ; DRY=$DRY"
[ -f $OUT/pairs.tsv ] || : > $OUT/pairs.tsv; [ -f $OUT/ctier.tsv ] || : > $OUT/ctier.tsv
one() {   # arm label
  local arm=$1 lab=$2 cw="$ARM_A" rc v tot ct P=$OUT/log/$2.plain; [ $arm = B ] && cw="$ARM_B"
  if [ "$STOP_EPOCH" -gt 0 ] && [ $(( $(date +%s) + RUN_S + 60 )) -gt "$STOP_EPOCH" ]; then rd_say "deadline: no run starts ($lab)"; return 8; fi
  if [ "$DRY" = 1 ]; then local b=0; [ $arm = B ] && b=1; { echo "VERIFY OK"; echo "rns_dist: RNS_STRATEGY=auto form B: C 29 products $((24 - 3 * b)).40 s, B 63 products 4.40 s, B4 0 products 0.00 s; extra plane memory 0.00 GiB/APU (allocations 0.00 s)"; echo "total   $((100 + RANDOM % 3 - b * 2)).10 s"; } > $OUT/log/$lab.log; rc=0
  else
    rd_run $lab $RUN_S srun --jobid=$HOLD --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env ECALC_SEGV_TRACE=1 $XE $cw ./ecalc $DPN" < /dev/null; rc=$?
  fi
  sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $P
  v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | awk '{print $2}')
  ct=$(grep -a -m1 '^rns_dist: RNS_STRATEGY' $P | sed -E 's/.*: C ([0-9]+) products ([0-9.]+) s.*/\1 \2/'); [ -n "$ct" ] || ct="nan nan"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" "$NODES" >> $OUT/pairs.tsv
  printf '%s\t%s\t%s\n' $lab $arm "$(echo $ct | tr ' ' '\t')" >> $OUT/ctier.tsv
  rd_say "$lab arm $arm rc $rc $v total $tot C-tier(products s) $ct"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ] || { rd_say "bad run $lab"; return 1; }
}
stats() {   # "n mean_delta half sig deltas" over complete rounds of the total; "all": the C-tier seconds too
  python3 - $OUT/pairs.tsv $OUT/ctier.tsv "$1" <<'P'
import sys,math
T={1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262}
def rounds(rows):
    R={}
    for lab,arm,val in rows:
        try: x=float(val)
        except: continue
        r=int(lab.split('_')[0][1:]); R.setdefault(r,{'A':[],'B':[]})[arm].append(x)
    return [sum(x['B'])/2-sum(x['A'])/2 for r,x in sorted(R.items()) if len(x['A'])==2 and len(x['B'])==2]
def ci(d):
    n=len(d)
    if n<2: return n,float('nan'),float('nan'),0
    m=sum(d)/n; sd=math.sqrt(sum((x-m)**2 for x in d)/(n-1)); se=sd/math.sqrt(n); half=(T.get(n-1,2.2))*se
    return n,m,half,1 if abs(m)>half else 0
rows=[l.rstrip('\n').split('\t') for l in open(sys.argv[1])]
d=rounds([(r[0],r[1],r[4]) for r in rows if len(r)>=5])
n,m,half,sig=ci(d)
if sys.argv[3]!='all':
    print(n,"%.2f"%m,"%.2f"%half,sig,"deltas",",".join("%.1f"%x for x in d)); sys.exit()
A=[float(r[4]) for r in rows if len(r)>=5 and r[1]=='A' and r[4].replace('.','').isdigit()]
print("total(s): n %d mean(B-A) %.2f 95%%CI half %.2f significant %d deltas %s ; arm A mean %.2f s"%(n,m,half,sig,",".join("%.1f"%x for x in d),sum(A)/len(A) if A else float('nan')))
st=[l.rstrip('\n').split('\t') for l in open(sys.argv[2])]
d=rounds([(r[0],r[1],r[3]) for r in st if len(r)>=4]); n,m,half,sig=ci(d)
cA=[float(r[3]) for r in st if len(r)>=4 and r[1]=='A' and r[3]!='nan']
print("C-tier(s): n %d mean(B-A) %.3f 95%%CI half %.3f significant %d ; arm A mean %.2f s (%s products)"%(n,m,half,sig,sum(cA)/len(cA) if cA else float('nan'),st[0][2] if st and len(st[0])>=3 else '?'))
P
}
r=$(python3 -c "import sys; R=set(int(l.split('\t')[0].split('_')[0][1:]) for l in open('$OUT/pairs.tsv') if l.strip()); print(max(R) if R else 0)")
[ $r -gt 0 ] && { awk -F'\t' -v r=$r '$1 !~ "^r" r "_"' $OUT/pairs.tsv > $OUT/p.tmp; cnt=$(grep -c "^r${r}_" $OUT/pairs.tsv); if [ $cnt -lt 4 ]; then mv $OUT/p.tmp $OUT/pairs.tsv; grep -v "^r${r}_" $OUT/ctier.tsv > $OUT/c.tmp; mv $OUT/c.tmp $OUT/ctier.tsv; r=$((r-1)); rd_say "dropped the incomplete round $((r+1))"; else rm -f $OUT/p.tmp; fi; }
why="MAXR"
while [ $r -lt $MAXR ]; do
  r=$((r+1)); if [ $((r % 2)) = 1 ]; then seq="A B B A"; else seq="B A A B"; fi; bad=0; i=0
  for arm in $seq; do i=$((i+1)); one $arm r${r}_${i}$arm; rc=$?; [ $rc = 0 ] || { bad=$rc; break; }; done
  [ $bad = 8 ] && { RD_VERDICT="deadline: round $r not started/finished; $(stats x)"; stats all > $OUT/stats.txt; exit 3; }
  [ $bad != 0 ] && { RD_VERDICT="FAILED: bad run in round $r (see $OUT/summary.txt)"; exit 1; }
  s=$(stats x); rd_say "after round $r: n mean(B-A) half-width(95%) significant: $s"
  set -- $s; n=$1; half=$3; sig=$4
  if [ $r -ge $MINR ]; then
    [ "$sig" = 1 ] && { why="p<0.05"; break; }
    awk -v h="$half" -v t="$HALF_S" 'BEGIN{exit !(h+0<=t+0)}' && { why="CI half-width <= $HALF_S s"; break; }
  fi
done
stats all > $OUT/stats.txt
RD_VERDICT="SUCCESS: K6 ABBA at $DPN digits done after $r rounds ($why): $(stats x)"
