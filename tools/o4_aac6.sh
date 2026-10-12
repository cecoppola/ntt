#!/bin/bash
# o4_aac6.sh <CPX|SPX> <start HH:MM Eastern (tomorrow if already past 12:00 now)> : fire-and-forget aac6 driver for O4 (tools/o4_rate.sh).
# CPX: node aac6-sh5cpx-b, 1e9 only (3 arms x 2 rounds).  SPX: a PPAC SPX node (any), 1e9 + 1e10 (needs the 1e10 memory of a 4-APU node).
# Own clone ~/ntt-o4 (built before arming), output ~/o4_$PART/ (summary.txt, rates.tsv, O4_DONE, ALERT).  Holds <= 45 min, cancelled by this script.
PART=${1:-CPX}; AT=${2:-03:31}; C=$HOME/ntt-o4; O=$HOME/o4_$PART; mkdir -p $O; rm -f $O/O4_DONE $O/ALERT
exec 9>$HOME/o4_$PART.lock; flock -n 9 || { echo "already running"; exit 1; }
TS=$(TZ=America/New_York date -d "$AT" +%s); [ $TS -lt $(date +%s) ] && TS=$((TS + 86400)) 
while [ $(date +%s) -lt $TS ]; do sleep 30; done
S() { echo "$(TZ=America/New_York date +%H:%M:%S) ET $*" >> $O/driver.txt; }
if [ $PART = CPX ]; then SB="-p SH5_MI300A_CPX -w aac6-sh5cpx-b"; ENVS="RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30"; PLAN="9"; else SB="-p PPAC_MI300A_SPX"; ENVS=""; PLAN="9 10"; fi
J=$(sbatch $SB -N1 --gpus=4 -t 0:45:00 -J o4$PART --parsable --wrap "sleep 2700" < /dev/null 2>$O/sbatch.err); J=${J%%;*}
[ -n "$J" ] || { echo "sbatch failed: $(tail -1 $O/sbatch.err)" > $O/ALERT; touch $O/O4_DONE; exit 1; }
trap 'scancel $J; touch $O/O4_DONE' EXIT
for i in $(seq 240); do [ "$(squeue -h -j $J -o %T)" = RUNNING ] && break; sleep 5; done
[ "$(squeue -h -j $J -o %T)" = RUNNING ] || { echo "hold $J not running after 20 min (abandoned, cancelled)" > $O/ALERT; exit 1; }
S "hold $J on $(squeue -h -j $J -o %N)"
MODE=aac6 J=$J NN=1 E=$C/ecalc U=$C/tools/unpack_digits O=$O PLAN="$PLAN" ROUNDS=2 CAP_S=2100 ENVS="$ENVS" REF9=$HOME/ntt/ecalc/results/e_1000000000.out REF10= bash $C/tools/o4_rate.sh >> $O/driver.txt 2>&1
S "driver ends"
