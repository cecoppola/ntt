#!/bin/bash
# s50_abba.sh (S50, from s40_abba.sh + s43_batch.sh) - ABBA of DIST_GATHER_TILE on aac7 inside a Slurm hold, NN nodes (1 or 4), no digit writes.
#   usage: s50_abba.sh <hold> <outdir> <NN> [digits-per-node]     (DRY=1: logic with stub runs, no Slurm)
#   env: WT (default ~/ntt-wt/s50; its ecalc/ecalc is used as built), MAXR (8), MINR (3), HALF_S (1.5 s: CI half-width stop), MARK (S50_DONE),
#        ARM_A (default "DIST_GATHER_TILE=0"), ARM_B (default "DIST_GATHER_TILE=32"), STOP_EPOCH (do not start a run that cannot end by it), RUN_S (expected wall, default 120*NN^0 -> 130 / 300)
# Run env = the aac7 headline LINE of ecalc/e16_headline.sh (parsed) + ECALC_SEGV_TRACE=1 + the arm words.  A round = 4 runs A B B A (odd) / B A A B (even);
# per-round delta = mean(B) - mean(A) of ecalc's own `total`.  After >= MINR rounds: stop at p < 0.05 (|mean| > t*se), CI half-width <= HALF_S, or MAXR.
# Marker <outdir>/<MARK> (SUCCESS / FAILED), table <outdir>/pairs.tsv, stats <outdir>/stats.txt.  Kills only rd_run's own srun PID; never scancel.
HOLD=$1; OUT=$2; NN=${3:-1}; DPN=${4:-64410000000}; DRY=${DRY:-0}
WT=${WT:-$HOME/ntt-wt/s50}; E=$WT/ecalc; MAXR=${MAXR:-8}; MINR=${MINR:-3}; HALF_S=${HALF_S:-1.5}; MARK=${MARK:-S50_DONE}
ARM_A=${ARM_A:-DIST_GATHER_TILE=0}; ARM_B=${ARM_B:-DIST_GATHER_TILE=32}; STOP_EPOCH=${STOP_EPOCH:-0}
if [ "$NN" = 1 ]; then RUN_S=${RUN_S:-130}; else RUN_S=${RUN_S:-300}; fi
source $WT/tools/rundriver.sh; rd_init $OUT $MARK
cd $E || exit 2
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$E/e16_headline.sh"); [ -n "$LINE" ] || { RD_VERDICT="FAILED: no LINE"; exit 2; }
if [ "$DRY" != 1 ]; then
  source aac7env.sh >/dev/null 2>&1; [ -n "$MNRUN_MODULES" ] || { RD_VERDICT="FAILED: no MNRUN_MODULES"; exit 2; }
  export SLURM_JOB_ID=$HOLD
  NODES=$(scontrol show hostnames "$(squeue -j $HOLD -h -o %N)" | head -$NN | tr '\n' ' ')
  [ "$(echo $NODES | wc -w)" = "$NN" ] || { RD_VERDICT="FAILED: hold $HOLD has fewer than $NN nodes"; exit 2; }
  [ -x $E/ecalc ] && grep -aqF DIST_GATHER_TILE $E/ecalc || { RD_VERDICT="FAILED: $E/ecalc missing or without DIST_GATHER_TILE"; exit 2; }
  [ "$NN" = 1 ] || ldd $E/ecalc | grep -q libsma || { RD_VERDICT="FAILED: ecalc not linked with SHMEM"; exit 2; }
else NODES="n1"; fi
NL=$(echo $NODES | tr ' ' ',')
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
rd_say "S50 ABBA: NN=$NN nodes $NL hold $HOLD digits/node $DPN ; A = $ARM_A ; B = $ARM_B ; stop epoch $STOP_EPOCH ; DRY=$DRY"
: > $OUT/pairs.tsv
one() {   # arm label
  local arm=$1 lab=$2 cw="$ARM_A" rc v tot P=$OUT/log/$2.plain; [ $arm = B ] && cw="$ARM_B"
  if [ "$STOP_EPOCH" -gt 0 ] && [ $(( $(date +%s) + RUN_S + 60 )) -gt "$STOP_EPOCH" ]; then rd_say "deadline: no run starts ($lab)"; return 8; fi
  if [ "$DRY" = 1 ]; then local b=0; [ $arm = B ] && b=1; { echo "VERIFY OK"; echo "total   $((100 + RANDOM % 3 - b * 2)).10 s"; } > $OUT/log/$lab.log; rc=0
  else
    rd_health $HOLD $NODES >/dev/null 2>&1 || { sleep 60; rd_health $HOLD $NODES >/dev/null 2>&1; } || { rd_say "unhealthy $NODES"; return 9; }
    rd_run $lab $RUN_S env MNRUN_NODES=$NN MNRUN_NODELIST=$NL MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 $cw ./ecalc $((NN * DPN)) < /dev/null; rc=$?
  fi
  sed -E 's/^ *[0-9]+: //' $OUT/log/$lab.log > $P
  v=$(rd_verify $P); tot=$(grep -a -m1 '^total' $P | awk '{print $2}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $lab $arm $rc "$v" "$tot" "$(TZ=America/New_York date +%m-%d_%H:%M:%S)" >> $OUT/pairs.tsv
  rd_say "$lab arm $arm rc $rc $v total $tot"
  [ $rc = 0 ] && [ "$v" = "VERIFY OK" ] && [ -n "$tot" ] || { rd_say "bad run $lab"; return 1; }
}
stats() {   # prints "n mean_delta half sig deltas" over complete rounds
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
  for arm in $seq; do i=$((i+1)); one $arm r${r}_${i}$arm; c=$?; [ $c = 0 ] || { bad=$c; break; }; done
  if [ $bad = 8 ]; then why="deadline"; r=$((r-1)); break; fi
  [ $bad != 0 ] && { RD_VERDICT="FAILED: bad run in round $r (see $OUT/summary.txt, $OUT/ALERT); partial: $(stats)"; exit 1; }
  s=$(stats); rd_say "after round $r: n mean(B-A) half-width(95%) significant: $s"
  set -- $s; half=$3; sig=$4
  if [ $r -ge $MINR ]; then
    [ "$sig" = 1 ] && { why="p<0.05"; break; }
    awk -v h="$half" -v t="$HALF_S" 'BEGIN{exit !(h+0<=t+0)}' && { why="CI half-width <= $HALF_S s"; break; }
  fi
done
stats > $OUT/stats.txt
RD_VERDICT="SUCCESS: S50 ABBA NN=$NN done after $r rounds ($why): $(stats)"
