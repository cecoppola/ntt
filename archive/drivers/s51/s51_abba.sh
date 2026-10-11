#!/bin/bash
# s51_abba.sh (S51, from s50_abba.sh) - paired ABBA of the S51 kernel switches (A = base, B = the kept switches) inside a Slurm hold, no digit writes.
#   usage: s51_abba.sh <hold> <outdir> <NN> [digits-per-node]      (DRY=1: the logic with stub runs, no Slurm)
#   env: PLAT (aac7 default | aac6), WT (default ~/ntt-wt/s51 on aac7, ~/ntt-s51 on aac6), MAXR (8), MINR (3), HALF_S (1.5 s CI stop), MARK (S51_DONE),
#        ARM_A (default "DBIG_S51_STATS=1"), ARM_B (default "DBIG_S51_STATS=1 DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5"), STOP_EPOCH (start no run that cannot end by it),
#        RUN_S (expected wall of one run: 130 for NN=1, 300 for 4), ROUND0 (first round number, to continue a series: rounds already in <outdir>/pairs.tsv are kept; default resume)
# aac7: run env = the headline LINE of ecalc/e16_headline.sh (parsed) via mnrun.sh, as s50_abba.sh.  aac6 (NN=1): srun --jobid --overlap, `module load rocm`, no LINE.
# A round = A B B A (odd) / B A A B (even); per-round delta = mean(B) - mean(A) of ecalc's `total`, and of the S51STAT times (addsub, modq, crtb: host wall seconds of the
# kernel stages, from DBIG_S51_STATS).  After >= MINR rounds: stop at p < 0.05 on the total, CI half-width <= HALF_S, or MAXR.  Resumable: rounds in pairs.tsv count.
# Marker <outdir>/<MARK> (SUCCESS / FAILED), tables <outdir>/pairs.tsv (+ s51stat.tsv), stats <outdir>/stats.txt.  Kills only rd_run's own srun PID; never scancel.
HOLD=$1; OUT=$2; NN=${3:-1}; DPN=${4:-64410000000}; DRY=${DRY:-0}; PLAT=${PLAT:-aac7}
if [ "$PLAT" = aac6 ]; then WT=${WT:-$HOME/ntt-s51}; else WT=${WT:-$HOME/ntt-wt/s51}; fi
E=$WT/ecalc; MAXR=${MAXR:-8}; MINR=${MINR:-3}; HALF_S=${HALF_S:-1.5}; MARK=${MARK:-S51_DONE}
ARM_A=${ARM_A:-DBIG_S51_STATS=1}; ARM_B=${ARM_B:-DBIG_S51_STATS=1 DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5}; STOP_EPOCH=${STOP_EPOCH:-0}
if [ "$NN" = 1 ]; then RUN_S=${RUN_S:-130}; else RUN_S=${RUN_S:-300}; fi
source $WT/tools/rundriver.sh; rd_init $OUT $MARK
cd $E || exit 2
if [ "$PLAT" = aac7 ]; then
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$E/e16_headline.sh"); [ -n "$LINE" ] || { RD_VERDICT="FAILED: no LINE"; exit 2; }
fi
if [ "$DRY" != 1 ]; then
  if [ "$PLAT" = aac7 ]; then
    source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { RD_VERDICT="FAILED: no MNRUN_MODULES"; exit 2; }
    NODES=$(scontrol show hostnames "$(squeue -j $HOLD -h -o %N)" | head -$NN | tr '\n' ' ')
    [ "$(echo $NODES | wc -w)" = "$NN" ] || { RD_VERDICT="FAILED: hold $HOLD has fewer than $NN nodes"; exit 2; }
    [ "$NN" = 1 ] || ldd $E/ecalc | grep -q libsma || { RD_VERDICT="FAILED: ecalc not linked with SHMEM"; exit 2; }
  else NODES=$(squeue -j $HOLD -h -o %N); fi
  export SLURM_JOB_ID=$HOLD
  [ -x $E/ecalc ] && grep -aqF DBIG_ADDSUB3 $E/ecalc || { RD_VERDICT="FAILED: $E/ecalc missing or without DBIG_ADDSUB3"; exit 2; }
else NODES="n1"; fi
NL=$(echo $NODES | tr ' ' ',')
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
rd_say "S51 ABBA: PLAT=$PLAT NN=$NN nodes $NL hold $HOLD digits/node $DPN ; A = $ARM_A ; B = $ARM_B ; stop epoch $STOP_EPOCH ; DRY=$DRY"
[ -f $OUT/pairs.tsv ] || : > $OUT/pairs.tsv; [ -f $OUT/s51stat.tsv ] || : > $OUT/s51stat.tsv
one() {   # arm label
  local arm=$1 lab=$2 cw="$ARM_A" rc v tot st P=$OUT/log/$2.plain; [ $arm = B ] && cw="$ARM_B"
  if [ "$STOP_EPOCH" -gt 0 ] && [ $(( $(date +%s) + RUN_S + 60 )) -gt "$STOP_EPOCH" ]; then rd_say "deadline: no run starts ($lab)"; return 8; fi
  if [ "$DRY" = 1 ]; then local b=0; [ $arm = B ] && b=1; { echo "VERIFY OK"; echo "S51STAT addsub 171 $((2 - b)).0100 modq 8 $((1 - b / 2)).2000 crtb 13 0.8200"; echo "total   $((100 + RANDOM % 3 - b * 2)).10 s"; } > $OUT/log/$lab.log; rc=0
  elif [ "$PLAT" = aac7 ]; then
    rd_health $HOLD $NODES >/dev/null 2>&1 || { sleep 60; rd_health $HOLD $NODES >/dev/null 2>&1; } || { rd_say "unhealthy $NODES"; return 9; }
    rd_run $lab $RUN_S env MNRUN_NODES=$NN MNRUN_NODELIST=$NL MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 $cw ./ecalc $((NN * DPN)) < /dev/null; rc=$?
  else
    rd_run $lab $RUN_S srun --jobid=$HOLD --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env ECALC_SEGV_TRACE=1 $cw ./ecalc $DPN" < /dev/null; rc=$?
  fi
  sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $P
  v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | awk '{print $2}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" "$NL" >> $OUT/pairs.tsv
  st=$(grep -a '^S51STAT' $P | awk '{a+=$4; m+=$7; c+=$10} END {printf "%.4f\t%.4f\t%.4f", a, m, c}'); printf '%s\t%s\t%s\n' $lab $arm "${st:-nan nan nan}" >> $OUT/s51stat.tsv
  rd_say "$lab arm $arm rc $rc $v total $tot S51STAT(add,modq,crtb) $st"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ] || { rd_say "bad run $lab"; return 1; }
}
stats() {   # prints "n mean_delta half sig deltas" over complete rounds of the total; stats_all prints the three kernel metrics too
  python3 - $OUT/pairs.tsv $OUT/s51stat.tsv "$1" <<'P'
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
d=rounds([(r[0],r[1],r[4]) for r in rows])
n,m,half,sig=ci(d)
if sys.argv[3]!='all':
    print(n,"%.2f"%m,"%.2f"%half,sig,"deltas",",".join("%.1f"%x for x in d)); sys.exit()
print("total(s): n %d mean(B-A) %.2f 95%%CI half %.2f significant %d deltas %s"%(n,m,half,sig,",".join("%.1f"%x for x in d)))
st=[l.rstrip('\n').split('\t') for l in open(sys.argv[2])]
for i,name in enumerate(["addsub","modq","crtb"]):
    d=rounds([(r[0],r[1],r[2+i]) for r in st if len(r)>=5]); n,m,half,sig=ci(d)
    print("%s(s): n %d mean(B-A) %.3f 95%%CI half %.3f significant %d"%(name,n,m,half,sig))
P
}
r=$(awk -F'\t' '{split($1,a,"_"); x=substr(a[1],2)+0; if (x>m) m=x} END {print m+0}' $OUT/pairs.tsv); why="MAXR"
# a partly done last round is redone: drop its rows
python3 - $OUT/pairs.tsv $OUT/s51stat.tsv <<'P'
import sys
for f in sys.argv[1:]:
    rows=[l for l in open(f)]; cnt={}
    for l in rows: r=l.split('_')[0]; cnt[r]=cnt.get(r,0)+1
    open(f,'w').writelines([l for l in rows if cnt[l.split('_')[0]]==4])
P
r=$(awk -F'\t' '{split($1,a,"_"); x=substr(a[1],2)+0; if (x>m) m=x} END {print m+0}' $OUT/pairs.tsv)
while [ $r -lt $MAXR ]; do
  r=$((r+1)); if [ $((r % 2)) = 1 ]; then seq="A B B A"; else seq="B A A B"; fi; i=0; bad=0
  for arm in $seq; do i=$((i+1)); one $arm r${r}_${i}$arm; c=$?; [ $c = 0 ] || { bad=$c; break; }; done
  if [ $bad = 8 ]; then why="deadline"; r=$((r-1)); break; fi
  [ $bad != 0 ] && { RD_VERDICT="FAILED: bad run in round $r (see $OUT/summary.txt, $OUT/ALERT); partial: $(stats)"; stats all > $OUT/stats.txt; exit 1; }
  s=$(stats); rd_say "after round $r: n mean(B-A) half-width(95%) significant: $s"
  set -- $s; half=$3; sig=$4
  if [ $r -ge $MINR ]; then
    [ "$sig" = 1 ] && { why="p<0.05"; break; }
    awk -v h="$half" -v t="$HALF_S" 'BEGIN{exit !(h+0<=t+0)}' && { why="CI half-width <= $HALF_S s"; break; }
  fi
done
stats all > $OUT/stats.txt
RD_VERDICT="SUCCESS: S51 ABBA PLAT=$PLAT NN=$NN done after $r rounds ($why): $(stats)"
