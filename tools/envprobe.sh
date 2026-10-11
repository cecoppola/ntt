#!/bin/bash
# envprobe.sh (C04, results/C04.md): a ~30-45 s environment probe of the fabric, independent of the arm under test.
# Stand-alone 4-thread all-to-all (tests/t_comm --bw) over all nodes of the hold at FIXED settings (the caller's COMM_/MN_/RNS_/ECALC_/DM_/
# FI_ variables are removed, then COMM_TRANSPORT=shmem FI_MR_CACHE_MONITOR=userfaultfd and the pool are set), 64 MiB and 256 MiB slabs;
# the NIC MCU counter group (oxe_mcu_meas_18..25 = A, 28..33 = B, the one that carries the bytes) and the flow-control stall counter
# (oxe_stall_fgfc_start_0, summed over the 4 NICs and all nodes) are read before and after on every node.
# Prints ONE line on stdout (and appends it to $OUT/envprobe.log):
#   ENVPROBE r64=<GB/s per thread> r256=<GB/s per thread> group=A|B|NA fgfc=<delta, millions> nn=<nodes> t=<epoch> wall=<s>
# (rate fields NA when unreadable).  Run it between runs, never concurrent with one (one network program per node).
#   usage: SLURM_JOB_ID=<hold> [NN=n] [WT=<worktree>] [OUT=dir] [REPS=100] [PMAX=256] [STUB=1] tools/envprobe.sh
# NOTE (A13, results/A13.md): the group is drawn per launch, so the probe's group/rate describe the probe's own launch, not the next
# run's; use it as a covariate of the fabric state (and of the run itself only through the run's own group, read the same way).
HD=$(cd "$(dirname "$0")" && pwd); WT=${WT:-$(dirname "$HD")}; STUB=${STUB:-0}
HOLD=${HOLD:-${SLURM_JOB_ID:-}}; REPS=${REPS:-100}; PMAX=${PMAX:-256}
OUT=${OUT:-$HOME/sess1011c/c04/probe}; mkdir -p "$OUT"
LAB=${LABEL:-ep$(date +%s)_$$}; t0=$(date +%s)
if [ $STUB = 1 ]; then NN=${NN:-4}; else
  : "${HOLD:?SLURM_JOB_ID/HOLD not set}"
  NN=${NN:-$(squeue -j "$HOLD" -h -o %D)}
fi
# node-side reader: "host A B F" (counter sums over cxi0..3)
NODE_READ='h=$(hostname -s); for c in 0 1 2 3; do d=/sys/class/cxi/cxi$c/device/telemetry; for k in 18 19 20 21 22 23 24 25 28 29 30 31 32 33; do echo "$k $(cut -d@ -f1 $d/oxe_mcu_meas_$k 2>/dev/null)"; done; echo "f $(cut -d@ -f1 $d/oxe_stall_fgfc_start_0 2>/dev/null)"; done | awk -v h=$h "{ if (\$1==\"f\") F+=\$2; else if (\$1<26) A+=\$2; else B+=\$2 } END{printf \"%s %.0f %.0f %.0f\n\", h, A, B, F}"'
snap() { # $1 = file, $2 = stub values (6 numbers)
  if [ $STUB = 1 ]; then printf "n0 %s %s %s\nn1 %s %s %s\n" ${2:-0 0 0 0 0 0} > "$1"; return; fi
  timeout 60 srun --jobid=$HOLD -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 bash -c "$NODE_READ" 2>/dev/null < /dev/null | grep -E '^[A-Za-z0-9_-]+ [0-9]+ [0-9]+ [0-9]+$' > "$1"; }
grpcalc() { # pre post -> "A|B|NA fgfc_millions"
  awk 'NR==FNR{a[$1]=$2; b[$1]=$3; f[$1]=$4; next} ($1 in a){dA+=$2-a[$1]; dB+=$3-b[$1]; dF+=$4-f[$1]; n++}
  END{ if(n==0 || dA+dB<=0) {print "NA NA"} else printf "%s %.1f\n", (dA>dB ? "A" : "B"), dF/1e6 }' "$1" "$2"; }
# helper modes for drivers that bracket a real run:  envprobe.sh --snap FILE | envprobe.sh --group PRE POST  (prints "A|B|NA fgfc_M")
case "${1:-}" in
  --snap) snap "$2" "${STUB_PRE:-0 0 0 0 0 0}"; exit 0;;
  --group) grpcalc "$2" "$3"; exit 0;;
esac
snap "$OUT/$LAB.pre" "${STUB_PRE:-0 0 0 0 0 0}"
log=$OUT/$LAB.log; : > $log
if [ $STUB = 1 ]; then
  echo "t_comm bw: pe 0 (rank 0 of 4): 67108864 B per slab: median 1 ms per exchange (min 1), ${STUB_R64:-12.0} GB/s per thread, aggregate 48 GB/s over 4 threads (1 s wall)" > $log
  echo "t_comm bw: pe 0 (rank 0 of 4): 268435456 B per slab: median 1 ms per exchange (min 1), ${STUB_R256:-10.4} GB/s per thread, aggregate 41 GB/s over 4 threads (1 s wall)" >> $log
else
  # fixed settings: drop every tunable the caller may have exported, then set ours
  while read -r v; do unset "$v"; done < <(env | cut -d= -f1 | grep -E '^(COMM|MN|RNS|ECALC|DM|DBIG|BS|DIST|NEWTON|FI)_')
  cd "$WT/ecalc" || exit 2
  ( exec env SLURM_JOB_ID=$HOLD MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_SHMEM_POOL_MB=12000 COMM_PORT=$((24000 + RANDOM % 900)) \
      timeout 240 ./mnrun.sh $NN env COMM_TRANSPORT=shmem FI_MR_CACHE_MONITOR=userfaultfd ./tests/t_comm --bw 4 $REPS $PMAX ) >> $log 2>&1 < /dev/null
fi
snap "$OUT/$LAB.post" "${STUB_POST:-100 10 5 100 10 5}"
rate() { sed -E 's/^ *[0-9]+: //' $log | grep -a "t_comm bw: pe [0-9]* (rank" | grep -a " $(( $1 * 1048576 )) B per slab" \
  | sed -nE 's/.*, ([0-9.]+) GB\/s per thread.*/\1/p' | awk '{v+=$1;n++} END{if(n>0) printf "%.2f", v/n; else print "NA"}'; }
R64=$(rate 64); R256=$(rate $PMAX)
read -r G F < <(grpcalc "$OUT/$LAB.pre" "$OUT/$LAB.post")
echo "ENVPROBE r64=$R64 r256=$R256 group=${G:-NA} fgfc=${F:-NA} nn=$NN t=$t0 wall=$(( $(date +%s) - t0 ))" | tee -a "$OUT/envprobe.log"
