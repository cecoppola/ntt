#!/bin/bash
# OWNER=T5
# EST_MIN=15
# S50 queue item: t_gather_probe (tile variants) + 1-node gates (1e9 digits) with DIST_GATHER_TILE=0 and =32/64 on the hold's first node. No rebuild.
H=$SLURM_JOB_ID; O=$HOME/batch1011/T5/s50; WT=$HOME/ntt-wt/s50; mkdir -p $O; cd $WT/ecalc || exit 2
source aac7env.sh >/dev/null 2>&1
srun --jobid=$H -N1 --gpus=4 -c 192 --overlap bash -lc "module unload rocm/7.0.3 >/dev/null 2>&1; module load $MNRUN_MODULES; cd $WT/ecalc; ./tests/t_gather_probe 8192 65536 8; ./tests/t_gather_probe 1024 524288 8" < /dev/null > $O/probe_item.log 2>&1
# pick the tile: the fastest probe variant (shape 1), only if it beats the 4-way-select kernel (the current product form) by >= 3 %
python3 - $O/probe_item.log > $O/TILE <<'P'
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
TB=$(cat $O/TILE)
for T in 0 $TB; do
  [ "$T" = skip ] && continue
  bash $WT/tools/gate.sh --nodes 1 --hold $H --out $O/gate_T$T --env "DIST_GATHER_TILE=$T" < /dev/null > $O/gate_T$T.log 2>&1
  cat $O/gate_T$T/gate.txt >> $O/gates.txt 2>/dev/null
done
grep -q PASS $O/gate_T$TB/gate.txt 2>/dev/null || echo skip > $O/TILE
squeue -s -j $H -h | grep -c . > $O/steps_left.txt
exit 0
