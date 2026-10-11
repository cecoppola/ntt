#!/bin/bash
# S45: the host-only many-rank matrix of the layered communicator, inside a SLURM allocation (or locally with HOSTS=localhost).
#   run_matrix.sh <t_lay> <outdir> <portbase> <N> <levels> <ubase> [--pools "ofi shmem"]
# Per variant (X2 = COMM_LAYER_VSLOT_POOL + COMM_LAYER_INTER2 off/on, SHARE off/on; the pool emulation ofi / shmem for X2) one run: the processes of node
# block k are forked by one t_lay per host (srun -N1 -w host) and every process prints a line; ana.py summarises.  Marker: <outdir>/<tag>.done.
BIN=$1; OUT=$2; PORT=$3; N=$4; LV=$5; UB=$6; POOLS=${7:-"ofi shmem"}
mkdir -p "$OUT"
if [ -n "${SLURM_JOB_NODELIST:-}" ]; then HOSTS=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' '); else HOSTS=${HOSTS:-localhost}; fi
set -- $HOSTS; H=$#; HL=$(echo $HOSTS | tr ' ' ',')
run() {   # tag x2 share pool
  local tag=$1 x2=$2 sh=$3 pk=$4 base=$5 k=0
  if [ $x2 = 1 ]; then export COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_INTER2=1; else unset COMM_LAYER_VSLOT_POOL COMM_LAYER_INTER2; fi
  export COMM_LAYER_VSLOT_SHARE=$sh
  : > "$OUT/$tag.txt"; local pids=""
  for h in $HOSTS; do
    local first=$((k * N / H)) last=$(((k + 1) * N / H)); k=$((k + 1))
    if [ -n "${SLURM_JOB_ID:-}" ]; then
      timeout 420 srun -N1 -n1 --overlap -w $h --export=ALL "$BIN" --nodes $N --levels $LV --hosts $HL --first $first --count $((last - first)) --port $base --ubase $UB --pool-mb 1024 --pool $pk >> "$OUT/$tag.txt" 2>&1 &
    else
      timeout 420 "$BIN" --nodes $N --levels $LV --hosts $HL --first $first --count $((last - first)) --port $base --ubase $UB --pool-mb 1024 --pool $pk >> "$OUT/$tag.txt" 2>&1 &
    fi
    pids="$pids $!"
  done
  wait $pids; echo "rc done $(date +%T)" > "$OUT/$tag.done"
  echo "$tag: $(grep -c '^T ' "$OUT/$tag.txt") process lines, $(grep -c 'FAIL' "$OUT/$tag.txt") FAIL lines, $(grep -c 'VERIFY OK' "$OUT/$tag.txt") host-OK lines"
}
b=$PORT
run n${N}_x0s0 0 0 ofi $b; b=$((b + 4500)); run n${N}_x0s1 0 1 ofi $b; b=$((b + 4500))
for pk in $POOLS; do run n${N}_x1s0_$pk 1 0 $pk $b; b=$((b + 4500)); run n${N}_x1s1_$pk 1 1 $pk $b; b=$((b + 4500)); done
python3 "$(dirname "$0")/ana.py" "$OUT"/n${N}_*.txt
