#!/bin/bash
# s51_aac6.sh (S51, T5b) - aac6 fire-and-forget driver, run on the aac6 login node: bash ~/ntt-s51/archive/drivers/s51/s51_aac6.sh   (setsid nohup, < /dev/null)
#   CPX worker  : sleeps to 03:30 EDT, one SH5 CPX job on s12-12 (<= 45 min): probe, t_dbig, 1-node gates (K2 K3 K4 all), ARM_B decision, short ABBA at 4e9 digits (coverage).
#   PPAC worker : sleeps to 03:50 EDT, then 42-min slots on PPAC s24-26 under  flock ~/batch1011/aac6_s24-26.lock  (shared with T5a): the first slot also runs the probe/gates
#                 stage; ABBA (6.441e10 digits, A = base, B = kept switches) resumes from <out>/abba/pairs.tsv in every slot until the stop rule / MAXR is met or D.
#   env: STUB=1 (logic only: no Slurm, DRY ABBA, no timers), WT (~/ntt-s51), O (~/batch1011/T5/s51b), D (1791723386), CPX_AT, PPAC_AT (epochs), MAXR (8), WHICH (cpx ppac).
# Markers: $O/CPX_DONE and $O/PPAC_DONE (first line SUCCESS/FAILED + reason), results in $O/{cpx,ppac}/ (abba/stats.txt, gates.txt, decide.txt).  Jobs: only ours (-J s51), cancelled by id.
STUB=${STUB:-0}; WT=${WT:-$HOME/ntt-s51}; E=$WT/ecalc; O=${O:-$HOME/batch1011/T5/s51b}; D=${D:-1791723386}; MAXR=${MAXR:-8}; WHICH=${WHICH:-"cpx ppac"}
CPX_AT=${CPX_AT:-$(date -d "2026-10-11 03:30 EDT" +%s)}; PPAC_AT=${PPAC_AT:-$(date -d "2026-10-11 03:50 EDT" +%s)}
[ "$STUB" = 1 ] && { CPX_AT=0; PPAC_AT=0; }
LOCK=${LOCK:-$HOME/batch1011/aac6_s24-26.lock}; mkdir -p $O $HOME/batch1011
say() { echo "$(TZ=America/New_York date '+%F %T %Z') [$1] $2" | tee -a $O/driver.log; }
until_epoch() { while [ $(date +%s) -lt $1 ]; do sleep 20; done; }
hold() {   # partition node minutes tag -> prints the job id once RUNNING (rc 0), else rc 1 (job cancelled)
  if [ "$STUB" = 1 ]; then echo 0; return 0; fi
  local J st i; J=$(sbatch -p $1 -w $2 -N1 --gpus=4 -t 0:$3:00 -J s51 --parsable --wrap "sleep $(( $3 * 60 ))" < /dev/null 2>$O/sbatch_$4.err); J=${J%%;*}; [ -n "$J" ] || return 1
  for i in $(seq 1 180); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && { echo $J; return 0; }; [ -z "$st" ] && return 1; sleep 5; done
  scancel $J; return 1
}
release() { [ "$STUB" = 1 ] || { [ "$1" != 0 ] && scancel $1 2>/dev/null; }; }
RS() {   # hold env-words cmd...  (srun into our hold; 10-min cap)
  local H=$1 XE=$2; shift 2
  if [ "$STUB" = 1 ]; then echo "STUB srun: $*"; else timeout 900 srun --jobid=$H --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; export $XE; $*" < /dev/null; fi
}
stage() {   # hold outdir logn pool-log XE-words : probe, t_dbig, gates, decision -> $2/ARM_B
  local H=$1 P=$2 L=$3 PL=$4 XE=$5 k n g e
  mkdir -p $P/probe $P/gates
  for k in 0 1; do
    RS $H "$XE" "./tests/t_s51_probe $L 7 $k" > $P/probe/probe_base_1$k.log 2>&1
    RS $H "$XE" "DBIG_ADDSUB3=1 ./tests/t_s51_probe $L 7 $k" > $P/probe/probe_k2_1$k.log 2>&1
    RS $H "$XE" "DBIG_MODQ_MONT=1 ./tests/t_s51_probe $L 7 $k" > $P/probe/probe_k3_1$k.log 2>&1
    RS $H "$XE" "./tests/t_s51_probe $L 7 $k" > $P/probe/probe_base_2$k.log 2>&1
    RS $H "$XE" "DBIG_ADDSUB3=1 ./tests/t_s51_probe $L 7 $k" > $P/probe/probe_k2_2$k.log 2>&1
    RS $H "$XE" "DBIG_MODQ_MONT=1 ./tests/t_s51_probe $L 7 $k" > $P/probe/probe_k3_2$k.log 2>&1
  done
  RS $H "$XE" "DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5 ./tests/t_dbig 24 0 $PL" > $P/t_dbig_on.log 2>&1; echo "t_dbig rc $?" >> $P/t_dbig_on.log
  for g in "k2:DBIG_ADDSUB3=1" "k3:DBIG_MODQ_MONT=1 ECALC_RES_LOG=1" "k4:RNS_CRT_SUB=5" "all:DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5"; do
    n=${g%%:*}; e=${g#*:}
    if [ "$STUB" = 1 ]; then echo "gate PASS | STUB" > $P/gates/gate_$n.txt; continue; fi
    timeout 600 bash $WT/tools/gate.sh --nodes 1 --hold $H --line none --env "$XE DBIG_S51_STATS=1 $e" --out $P/gates/g_$n --timeout 300 < /dev/null > $P/gates/gate_$n.log 2>&1
    cp $P/gates/g_$n/gate.txt $P/gates/gate_$n.txt 2>/dev/null || echo "gate FAIL | no summary" > $P/gates/gate_$n.txt
  done
  grep -q "rc 0" $P/t_dbig_on.log && grep -q "gate PASS" $P/gates/gate_all.txt 2>/dev/null || echo "S51: t_dbig or combined gate not clean" >> $P/decide.txt
  python3 $WT/archive/drivers/s51/s51_decide.py $P/probe $P/gates > $P/ARM_B 2>> $P/decide.txt
  cat $P/gates/gate_*.txt >> $P/gates.txt
}
abba() {   # hold outdir digits stop_epoch run_s half_s XE-words -> rc 0 when finished (rule met / MAXR), 3 when the slot ran out (resume later), 1 failed
  local H=$1 P=$2 DPN=$3 STOP=$4 RUNS=$5 HALF=$6 XE=$7 B v
  B=$(cat $P/ARM_B 2>/dev/null); if [ -z "$B" ] || [ "$B" = "DBIG_S51_STATS=1" ]; then echo "no kernel kept (ARM_B='$B')" > $P/abba_skipped; return 0; fi
  DRY=$STUB PLAT=aac6 WT=$WT MAXR=$MAXR MINR=3 HALF_S=$HALF MARK=S51_DONE STOP_EPOCH=$STOP RUN_S=$RUNS ARM_A="DBIG_S51_STATS=1 $XE" ARM_B="$B $XE" \
    bash $WT/archive/drivers/s51/s51_abba.sh $H $P/abba 1 $DPN < /dev/null
  v=$(head -1 $P/abba/S51_DONE 2>/dev/null); case "$v" in *deadline*) return 3;; SUCCESS*) return 0;; *) return 1;; esac
}
cpx_worker() {
  local P=$O/cpx H end st; mkdir -p $P; until_epoch $CPX_AT; say cpx "start"
  H=$(hold SH5_MI300A_CPX aac6-sh5cpx-b 45 cpx) || { say cpx "no hold"; echo "FAILED: no CPX hold" > $O/CPX_DONE; return; }
  end=$(( $(date +%s) + 45 * 60 )); [ $end -gt $D ] && end=$D
  say cpx "hold $H; stage"; stage $H $P 27 29 "RNS_INIT_POOL_LOG=29"; say cpx "ARM_B: $(cat $P/ARM_B)"
  abba $H $P 4000000000 $(( end - 120 )) 60 0.15 "RNS_INIT_POOL_LOG=29"; st=$?
  release $H; [ "$(squeue -s -j $H -h 2>/dev/null | wc -l)" = 0 ] || say cpx "WARNING: steps left in $H"
  say cpx "done rc $st: $(head -1 $P/abba/S51_DONE 2>/dev/null)"; { [ $st = 1 ] && echo "FAILED: $(head -1 $P/abba/S51_DONE 2>/dev/null)" || echo "SUCCESS: $(head -1 $P/abba/S51_DONE 2>/dev/null) ARM_B=$(cat $P/ARM_B)"; } > $O/CPX_DONE
}
ppac_worker() {
  local P=$O/ppac H end mins st=3 slot=0; mkdir -p $P; until_epoch $PPAC_AT
  while [ $st = 3 ] && [ $(( $(date +%s) + 20 * 60 )) -lt $D ]; do
    exec 9> $LOCK; say ppac "waiting for the lock"; flock 9; slot=$((slot + 1))
    mins=42; [ $(( $(date +%s) + (mins + 3) * 60 )) -gt $D ] && mins=$(( (D - $(date +%s)) / 60 - 3 ))
    if [ $mins -lt 15 ]; then say ppac "no time for a slot"; flock -u 9; exec 9>&-; break; fi
    H=$(hold PPAC_MI300A_SPX aac6-spx-b $mins ppac) || { say ppac "slot $slot: no hold"; flock -u 9; exec 9>&-; sleep 60; continue; }
    end=$(( $(date +%s) + mins * 60 )); say ppac "slot $slot: hold $H, $mins min"
    [ -s $P/ARM_B ] || { stage $H $P 28 31 "X51=1"; say ppac "ARM_B: $(cat $P/ARM_B)"; }
    abba $H $P 64410000000 $(( end - 150 )) 130 1.0 "X51=1"; st=$?
    release $H; [ "$(squeue -s -j $H -h 2>/dev/null | wc -l)" = 0 ] || say ppac "WARNING: steps left in $H"
    say ppac "slot $slot rc $st: $(head -1 $P/abba/S51_DONE 2>/dev/null)"
    flock -u 9; exec 9>&-; sleep 5
  done
  { [ $st = 1 ] && echo "FAILED: $(head -1 $P/abba/S51_DONE 2>/dev/null)" || echo "$([ $st = 3 ] && echo PARTIAL || echo SUCCESS): $(head -1 $P/abba/S51_DONE 2>/dev/null) ARM_B=$(cat $P/ARM_B 2>/dev/null)"; } > $O/PPAC_DONE
}
for w in $WHICH; do ${w}_worker & done; wait
