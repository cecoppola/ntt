#!/bin/bash
# OWNER=c02
# NODES=4
# EST_MIN=12
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
# c02 (results/C02.md "Microbenchmark"): peer-schedule probe t_a2a_sched (stand-alone Cray SHMEM, no engine code) on 4 nodes:
# L launches; each launch runs the schedules seq / rot / rr / win2 / pair at 16 and 64 MiB per peer in ONE process (paired within
# the launch, so the launch's NIC MCU group -- A13: the slow mode -- is the same for every schedule), g = 4 (3 peers) and every
# other launch also g = 2.  NIC counters oxe_mcu_meas_18..33 and oxe_stall_fgfc_start_0 are read on every node before and after
# each launch (A13's grouping: group A = 18..25 moved, group B = 28..33 moved).  Output: ~/sess1011c/c02/R4_<epoch>/ (launch
# logs, nic_*.txt, summary.txt, DONE).  Exits by itself; never cancels the hold.  STUB=1 / AQ_STUB=1: no srun, logic only.
set -u
BIN=$HOME/ntt-wt/c02/ecalc/tests/t_a2a_sched                   # built on the login node (results/C02.md "Build")
L=${C02_LAUNCHES:-12}; N=${AQ_NODES:-4}
STUB=${AQ_STUB:-${STUB:-0}}
S=$HOME/sess1011c/c02; OUT=$S/R${N}_$(date +%s); mkdir -p $OUT
log() { echo "$(TZ=America/New_York date '+%H:%M:%S') $*" | tee -a $OUT/drive.log; }
if [ "$STUB" = 1 ]; then
    srun() { echo "[stub srun] $*"; cat <<EOF
t_a2a_sched: 16 PEs on 4 nodes, meshes of g = 4 (3 peers), chunk 4 MiB, 10 reps, orders seq,rot,rr,win2,pair
t_a2a_sched: pe 0 node 0 (stub) loc 0 mesh rank 0 of 4: slab    64 MiB order seq  : median   10.000 ms (min    9.000)  20.13 GB/s per PE, loop  19.00 GB/s
t_a2a_sched: pe 1 node 0 (stub) loc 1 mesh rank 0 of 4: slab    64 MiB order rr   : median    9.000 ms (min    8.000)  22.37 GB/s per PE, loop  21.00 GB/s
stub cxi0 mcu_18 100@1.0
stub cxi0 fgfc 5@1.0
t_a2a_sched: VERIFY OK (0 bad bytes over all PEs)
EOF
    }
    J=stub
else
    : "${SLURM_JOB_ID:?need the runner hold}"; J=$SLURM_JOB_ID
    [ -x "$BIN" ] || { log "FATAL: $BIN missing (build it on the login node first)"; echo FAILED > $OUT/DONE; exit 2; }
fi
SR="srun --jobid=$J -N$N --overlap --mem=0 --gpus-per-node=4 --export=ALL"
snap() {   # snap <tag>: one line per node x NIC x counter, value@timestamp as the kernel prints it
    $SR --ntasks=$N --ntasks-per-node=1 -c 1 bash -c 'h=$(hostname -s); for c in 0 1 2 3; do t=/sys/class/cxi/cxi$c/device/telemetry; for k in 18 19 20 21 22 23 24 25 28 29 30 31 32 33; do echo "$h cxi$c mcu_$k $(cat $t/oxe_mcu_meas_$k 2>/dev/null | tr "\n" " ")"; done; echo "$h cxi$c fgfc $(cat $t/oxe_stall_fgfc_start_0 2>/dev/null | tr "\n" " ")"; done' > $OUT/nic_$1.txt 2>&1
}
log "start: hold $J, $N nodes, $L launches, bin $BIN, out $OUT"
for ((i = 1; i <= L; i++)); do
    tag=$(printf "L%02d" $i)
    snap ${tag}_pre
    g=$N; if [ $((i % 2)) = 0 ] && [ $N -ge 4 ]; then g=2; fi
    SHMEM_SYMMETRIC_SIZE=1500M $SR --ntasks=$((4 * N)) --ntasks-per-node=4 -c 48 --distribution=block \
        $BIN -S 16,64 -c 4 -r 10 -g $g -o seq,rot,rr,win2,pair > $OUT/$tag.log 2>&1; rc=$?
    snap ${tag}_post
    log "$tag g=$g rc $rc $(grep -c 'order' $OUT/$tag.log) lines, $(grep -m1 VERIFY $OUT/$tag.log)"
done
# summary: per launch the MCU group (which counter set moved on cxi0 of node 0 between pre and post) and the per-order mean GB/s per PE
{
echo "launch g group fgfc_delta | mean GB/s per PE over PEs (median per exchange): slab order rate ..."
for ((i = 1; i <= L; i++)); do
    tag=$(printf "L%02d" $i); pre=$OUT/nic_${tag}_pre.txt; post=$OUT/nic_${tag}_post.txt
    grp=$(awk 'FNR==NR { if ($3 ~ /^mcu_/) { split($4, v, "@"); p[$1 " " $2 " " $3] = v[1] } ; next }
               $3 ~ /^mcu_/ { split($4, v, "@"); d = v[1] - p[$1 " " $2 " " $3]; k = substr($3, 5) + 0; if (k <= 25) a += d; else b += d }
               END { printf "%s(A=%.3g,B=%.3g)", (a > b ? "A" : (b > 0 ? "B" : "?")), a, b }' $pre $post 2>/dev/null)
    fg=$(awk 'FNR==NR { if ($3 == "fgfc") { split($4, v, "@"); p[$1 " " $2] = v[1] }; next } $3 == "fgfc" { split($4, v, "@"); d += v[1] - p[$1 " " $2] } END { printf "%.3g", d }' $pre $post 2>/dev/null)
    g=$(grep -m1 -o "meshes of g = [0-9]*" $OUT/$tag.log | awk '{print $5}')
    rates=$(awk '/ order / { for (k = 1; k <= NF; k++) if ($k == "slab") s = $(k + 1); for (k = 1; k <= NF; k++) if ($k == "order") o = $(k + 1); sub(/:$/, "", o); for (k = 1; k <= NF; k++) if ($k == "GB/s" && $(k + 1) == "per") r = $(k - 1); n[s " " o]++; t[s " " o] += r }
                 END { for (x in n) printf "%s %.2f  ", x, t[x] / n[x] }' $OUT/$tag.log | tr ' ' ' ')
    echo "$tag g=$g $grp fgfc=$fg | $rates"
done
echo
echo "per (g, slab, order): mean GB/s per PE over launches and PEs; launches grouped by MCU group"
awk 'FNR == 1 { split(FILENAME, a, "/"); } /meshes of g/ { g = $11 } / order / { for (k = 1; k <= NF; k++) if ($k == "slab") s = $(k + 1); for (k = 1; k <= NF; k++) if ($k == "order") o = $(k + 1); sub(/:$/, "", o); for (k = 1; k <= NF; k++) if ($k == "GB/s" && $(k + 1) == "per") r = $(k - 1); key = "g=" g " slab=" s " " o; n[key]++; t[key] += r; if (!(key in mn) || r < mn[key]) mn[key] = r; if (r > mx[key]) mx[key] = r }
     END { for (x in n) printf "%-26s n %4d mean %6.2f min %6.2f max %6.2f GB/s per PE\n", x, n[x], t[x] / n[x], mn[x], mx[x] }' $OUT/L*.log | sort
} > $OUT/summary.txt.tmp && mv $OUT/summary.txt.tmp $OUT/summary.txt
cat $OUT/summary.txt | tee -a $OUT/drive.log
echo "OK $(date +%s)" > $OUT/DONE
log "done: $OUT/summary.txt"
exit 0
