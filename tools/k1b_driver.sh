#!/bin/bash
# tools/k1b_driver.sh (K1b, results/K1b.md; aac6 PPAC_MI300A_SPX, one node): fire-and-forget, run as
#   setsid nohup bash ~/ntt-k1b/tools/k1b_driver.sh > ~/k1b/driver.log 2>&1 < /dev/null &
# Steps: own sbatch sleep job (45 min, queued behind our other jobs; gives up at GIVEUP_EPOCH and cancels only its own id) ->
#   1 probe tests/t_gather_raw_probe at the product shape (rows 8192 x C 65536) and at rows 1024 x C 524288;
#     model saving = 174 calls x 36.5 ms x (1 - RATIO4) (O7b: 174 k_gather calls per APU at 6.441e10, 36.5 ms each, measured;
#     RATIO4 = raw-once / now per operand at np = 4, measured by the probe); stop (SUCCESS: refuted) if < 1.5 s
#   2 tools/gate.sh --build --nodes 1 with RNS_STRATEGY=C (1e9 is all B form by default; C forces the product that k1b changes)
#     and DIST_GATHER_RAW=2, then without --build for =1 and =0
#   3 ABBA at 6.441e10 (A = DIST_GATHER_RAW=0, B = 2), tools/s50_abba6.sh: p<0.05 after >= 3 rounds, cap 5, or the hold's end
#   4 device memory per arm from the runs' mem_report lines; summary.txt, DONE marker; scancel of its own job; no step left
# Files: ~/k1b/{summary.txt,DONE,probe.log,gate_*,abba/}.  STOP file ~/k1b/STOP: no new step starts.
O=$HOME/k1b; WT=$HOME/ntt-k1b; mkdir -p $O; rm -f $O/DONE
GIVEUP_EPOCH=${GIVEUP_EPOCH:-0}; DIG=64410000000
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S') $*" >> $O/summary.txt; }
MSG="FAILED: driver exited early"; J=
fin() { MSG="$*"; exit 0; }
final() { [ -n "$J" ] && { scancel $J 2>/dev/null; sleep 5; say "scancel $J; steps left: $(squeue -s -j $J -h 2>/dev/null | wc -l)"; }; say "DONE: $MSG"; echo "$MSG" > $O/DONE; }
trap final EXIT
stopped() { [ -f $O/STOP ] && fin "FAILED: STOP file"; }
say "start (head $(git -C $WT log --oneline -1 | cut -c1-12))"
JJ=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J k1b --parsable --wrap "sleep 2700" < /dev/null) || fin "FAILED: sbatch"
J=${JJ%%;*}; echo $J > $O/jobid; say "job $J submitted"
while :; do
  st=$(squeue -j $J -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break
  [ -z "$st" ] && fin "FAILED: job $J vanished"
  [ "$GIVEUP_EPOCH" -gt 0 ] && [ $(date +%s) -gt $GIVEUP_EPOCH ] && fin "FAILED: job $J not started by the give-up time (cancelled)"
  [ -f $O/STOP ] && fin "FAILED: STOP file while pending (cancelled)"
  sleep 60
done
T0=$(date +%s); STOP=$(( T0 + 45 * 60 - 150 )); say "running (hold ends ~$(TZ=America/New_York date -d @$((T0 + 2700)) +%H:%M))"
export SRUNX="--gpus=4"
# 1 probe
timeout 600 srun --jobid=$J --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $WT/ecalc; ./tests/t_gather_raw_probe 8192 65536 6; ./tests/t_gather_raw_probe 1024 524288 6" < /dev/null > $O/probe.log 2>&1
say "probe: $(grep -a -E 'best|RATIO4|FAILED' $O/probe.log | sed 's/  */ /g' | tr '\n' ';')"
grep -q FAILED $O/probe.log && fin "FAILED: probe mismatch (see probe.log)"
R4=$(grep -a -m1 RATIO4 $O/probe.log | awk '{print $2}'); [ -n "$R4" ] || fin "FAILED: probe gave no RATIO4"
SAVE=$(awk -v r=$R4 'BEGIN { printf "%.2f", 174 * 0.0365 * (1 - r) }'); say "model: RATIO4 $R4 -> saving $SAVE s per run at 6.441e10 (modelled, all gather calls covered, np 4)"
awk -v s=$SAVE 'BEGIN { exit !(s >= 1.5) }' || fin "SUCCESS: refuted by the probe (modelled saving $SAVE s < 1.5 s); nothing timed"
stopped
# 2 gates (1e9, RNS_STRATEGY=C so that the C-form dist_core runs)
gok=1; b=--build
for G in 2 1 0; do
  bash -lc "module load rocm >/dev/null 2>&1; cd $WT && tools/gate.sh $b --nodes 1 --hold $J --env 'RNS_STRATEGY=C DIST_GATHER_RAW=$G' --out $O/gate_$G --timeout 600" < /dev/null > $O/gate_$G.out 2>&1; rc=$?
  say "gate DIST_GATHER_RAW=$G (RNS_STRATEGY=C, 1e9${b:+, build}): rc $rc: $(cat $O/gate_$G/gate.txt 2>/dev/null | cut -c1-200)"
  [ $rc = 0 ] || gok=0; b=
  [ $gok = 1 ] || break
done
[ $gok = 1 ] || fin "FAILED: gate (see summary.txt)"
stopped
# 3 ABBA
WT=$WT PRE="X=1" STOP_EPOCH=$STOP RUN_S=110 MINR=3 MAXR=5 bash $WT/tools/s50_abba6.sh $J $O/abba "DIST_GATHER_RAW=0" "DIST_GATHER_RAW=2" $DIG
RES=$(cat $O/abba/RESULT 2>/dev/null); say "ABBA (A = DIST_GATHER_RAW=0, B = 2, 6.441e10): $RES"
say "runs: $(cut -f1-5 $O/abba/pairs.tsv | tr '\t' ' ' | tr '\n' ';')"
# 4 memory per arm: the largest 'device X GB in use' and 'peak live' of the block pool over the run's mem_report lines
python3 - $O/abba <<'P' >> $O/summary.txt
import re,sys,glob,os
D=sys.argv[1]; arm={}
for l in open(D+'/pairs.tsv'): f=l.split('\t'); arm[f[0]]=f[1]
for a in 'AB':
    dev=[];pk=[];hm=[];fit=0
    for lab,x in arm.items():
        if x!=a: continue
        t=open(f'{D}/log/{lab}.log',errors='replace').read()
        for m in re.finditer(r'device ([0-9.]+) GB in use \(per APU ([0-9.]+)\.\.([0-9.]+)\).*?hipMalloc ([0-9.]+); live [0-9.]+, peak live ([0-9.]+)',t):
            dev.append(float(m.group(1))); hm.append(float(m.group(4))); pk.append(float(m.group(5)))
        fit+=t.count('DIST_GATHER_RAW: APU')
    print(f"memory arm {a}: device in use max {max(dev) if dev else 'n/a'} GB (node), block pool peak live max {max(pk) if pk else 'n/a'} GB, pool hipMalloc max {max(hm) if hm else 'n/a'} GB, 'does not fit' notes {fit}")
P
case "$RES" in DONE*) fin "SUCCESS: probe RATIO4 $R4 (saving $SAVE s modelled); gates PASS; $RES";; *) fin "FAILED: ABBA: $RES";; esac
