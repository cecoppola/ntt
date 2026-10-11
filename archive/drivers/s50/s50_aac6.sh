#!/bin/bash
# s50_aac6.sh <cpx|ppac> [START_EPOCH]   (S50, aac6; run from a sleep-until timer: setsid nohup, never a pending Slurm job through the blackout)
#   cpx : SH5_MI300A_CPX node aac6-sh5cpx-a, 4e9 digits, POOL_LOG=29 (coverage).   ppac : PPAC_MI300A_SPX node aac6-spx-b (shared with T5b through flock), 6.441e10 digits.
#   Steps: sleep until START; (ppac: flock ~/batch1011/aac6_s24-26.lock for the whole job); own sbatch sleep job (<= 40 min, ends by D, scancel only that id);
#   ppac: t_gather_probe -> TILE (fastest of 16/32/64 if >= 3 % faster than the select kernel, else skip); gates 1e9 (TILE=0 and TILE): VERIFY OK + digcmp identical to
#   ~/ntt/ecalc/results/e_1000000000.out; ABBA (A = DIST_GATHER_TILE=0, B = tile) with s50_abba6.sh, stop p<0.05 / MAXR 5 / the job end.  cpx: no probe (needs 4 APUs): TILE from
#   $S50_TILE (default 32).  Marker ~/batch1011/T5/aac6_<kind>/DONE, summary there.
KIND=$1; START=${2:-0}; D=1791723386
WT=$HOME/ntt-s50; O=$HOME/batch1011/T5/aac6_$KIND; mkdir -p $O; rm -f $O/DONE
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S') $*" >> $O/summary.txt; }
if [ $KIND = cpx ]; then PART=SH5_MI300A_CPX; NODE=aac6-sh5cpx-a; DIG=4000000000; PRE="POOL_LOG=29"; GATEPRE="POOL_LOG=29"; RUN_S=40
else PART=PPAC_MI300A_SPX; NODE=aac6-spx-b; DIG=64410000000; PRE="X=1"; GATEPRE="X=1"; RUN_S=100; fi
SX=; [ $KIND = ppac ] && SX="--gpus-per-node=4"; export SRUNX=$SX
while [ $(date +%s) -lt $START ]; do sleep 30; done
say "start $KIND (epoch $(date +%s))"
if [ $KIND = ppac ]; then exec 9> $HOME/batch1011/aac6_s24-26.lock; flock 9; say "lock acquired"; fi
now=$(date +%s); LEFT=$(( D - now )); [ $LEFT -ge 1500 ] || { echo "FAILED: too late ($LEFT s to D)" > $O/DONE; exit 1; }
TL=${TLMAX:-40}; [ $LEFT -lt $((40 * 60 + 120)) ] && TL=$(( (LEFT - 120) / 60 ))
J=$(sbatch -p $PART -N1 -w $NODE --gpus=4 $( [ $KIND = cpx ] && echo --exclusive ) -t 0:$TL:00 -J s50$KIND --parsable --wrap "sleep $((TL * 60))" < /dev/null) || { echo "FAILED: sbatch" > $O/DONE; exit 1; }
J=${J%%;*}; echo $J > $O/jobid; say "job $J (-t $TL min) on $NODE"
trap 'scancel $J 2>/dev/null; say "scancel $J; steps left: $(squeue -s -j $J -h 2>/dev/null | wc -l)"' EXIT
for i in $(seq 1 60); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { echo "FAILED: job vanished" > $O/DONE; exit 1; }; sleep 10; done
[ "$st" = RUNNING ] || { echo "FAILED: job $J not running after 10 min (cancelled)" > $O/DONE; exit 1; }
JEND=$(( $(date +%s) + TL * 60 - 120 )); STOP=$(( JEND < D ? JEND : D ))
say "running; stop epoch $STOP"
run() { # label digits outfile|- env...
  local lab=$1 d=$2 f=$3; shift 3; [ "$f" = - ] && f=""
  timeout 450 srun --jobid=$J $SX --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $WT/ecalc; env $* ./ecalc $d $f" < /dev/null > $O/$lab.log 2>&1; }
TILE=${S50_TILE:-32}
if [ $KIND = ppac ]; then
  timeout 600 srun --jobid=$J $SX --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $WT/ecalc; ./tests/t_gather_probe 8192 65536 8; ./tests/t_gather_probe 1024 524288 8" < /dev/null > $O/probe.log 2>&1
  TILE=$(python3 - $O/probe.log <<'P'
import re,sys
t=open(sys.argv[1]).read().split("rows 1024")[0]
sel=None; best=None
for l in t.splitlines():
    m=re.search(r"best ([0-9.]+) ms",l)
    if not m: continue
    v=float(m.group(1))
    if "4-way select" in l and "remote" in l: sel=v
    mt=re.search(r"tile (\d+)",l)
    if mt and (best is None or v<best[0]): best=(v,mt.group(1))
print("skip" if ("FAILED" in t or not (sel and best and best[0]<0.97*sel)) else best[1])
P
)
  say "probe -> TILE=$TILE: $(grep -a 'best' $O/probe.log | head -8 | sed 's/  */ /g' | tr '\n' ';')"
fi
[ -n "$TILE" ] || TILE=skip
if [ "$TILE" != skip ]; then
  gok=1
  for T in 0 $TILE; do
    f=$O/gate_$T.txt; run gate_$T 1000000000 $f $GATEPRE DIST_GATHER_TILE=$T
    v=$(grep -a -o 'VERIFY [A-Z]*' $O/gate_$T.log | tail -1); c=$(bash $WT/ecalc/digcmp.sh $f $HOME/ntt/ecalc/results/e_1000000000.out 2>&1 | tail -1); rm -f $f $f.*
    say "gate T=$T: $v, digits $c, $(grep -a -m1 '^total' $O/gate_$T.log | cut -c1-60)"
    [ "$v" = "VERIFY OK" ] && [ "$c" = identical ] || gok=0
  done
  if [ $gok = 1 ]; then
    WT=$WT PRE="$PRE" STOP_EPOCH=$STOP RUN_S=$RUN_S MINR=3 MAXR=5 bash $WT/archive/drivers/s50/s50_abba6.sh $J $O/abba "DIST_GATHER_TILE=0" "DIST_GATHER_TILE=$TILE" $DIG
    say "ABBA: $(cat $O/abba/RESULT 2>/dev/null)"
    echo "SUCCESS: tile $TILE gates passed; $(cat $O/abba/RESULT 2>/dev/null)" > $O/DONE
  else echo "FAILED: gate (see summary)" > $O/DONE; fi
else echo "SUCCESS: no tile beat the select kernel in the probe (TILE=skip); nothing timed" > $O/DONE; fi
