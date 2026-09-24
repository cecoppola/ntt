#!/bin/bash
# tests/plan_validate.sh <node> <outdir> - Phase 13d L: MN_PLAN_ONLY against real runs (RNS_VERBOSE=1 product lines) on one node:
# size 1 at 10^9 and 10^10, sizes 2, 3, 4 at 10^9 (POOL_LOG=29, as mnaccept's mn step), size 4 at 10^10 (POOL_LOG=29, the M-run's),
# then the same with the plane cap lowered (DIST_LOGN_TEST) so the grids and the cuts show at these sizes.  Allocates its own
# 45-minute job, runs unattended (setsid nohup), cancels the job at the end.  One compare table per run in <outdir>.
node=$1; OUT=$2; cd "$(dirname "$0")/.." || exit 2
mkdir -p "$OUT"; LOG=$OUT/batch.log
exec >> "$LOG" 2>&1
J=$(sbatch --parsable -p PPAC_MI300A_SPX -N1 -w "$node" --gpus=4 -t 0:45:00 -J L --wrap "sleep 2700")
echo "job $J on $node, $(date)"
for i in $(seq 1 360); do [ "$(squeue -j "$J" -h -o %T)" = RUNNING ] && break; sleep 5; done
[ "$(squeue -j "$J" -h -o %T)" = RUNNING ] || { echo "job $J never ran"; scancel "$J"; exit 1; }
echo "running $(date)"
R() { local tag=$1 to=$2; shift 2; timeout "$to" srun --jobid="$J" -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $PWD; env $*" > "$OUT/$tag.log" 2>&1; echo "$tag rc $? $(grep -a '^total\|VERIFY' "$OUT/$tag.log" | tail -2 | tr '\n' ' ') $(date +%T)"; }
M() { local tag=$1 to=$2 p=$3; shift 3; SLURM_JOB_ID=$J timeout "$to" ./mnrun.sh "$p" env "$@" > "$OUT/$tag.log" 2>&1; echo "$tag rc $? $(grep -a 'mn: all\|^total' "$OUT/$tag.log" | tail -2 | tr '\n' ' ') $(date +%T)"; }
P() { local tag=$1 spec=$2; shift 2; env "$@" MN_PLAN_ONLY=$spec ./ecalc > "$OUT/$tag.plan" 2>&1; python3 tests/plan_compare.py "$OUT/$tag.log" "$OUT/$tag.plan" > "$OUT/$tag.cmp"; echo "  compare $tag: $(tail -1 "$OUT/$tag.cmp")"; }
V="RNS_VERBOSE=1 NEWTON_VERBOSE=1"
R s1_e9 300 $V ./ecalc 1000000000;                                   P s1_e9 1000000000:1
M mn2_e9 600 2 POOL_LOG=29 $V ./ecalc 1000000000;                    P mn2_e9 1000000000:2 POOL_LOG=29
M mn3_e9 600 3 POOL_LOG=29 $V ./ecalc 1000000000;                    P mn3_e9 1000000000:3 POOL_LOG=29
M mn4_e9 600 4 POOL_LOG=29 $V ./ecalc 1000000000;                    P mn4_e9 1000000000:4 POOL_LOG=29
R s1_e10 400 $V ./ecalc 10000000000;                                 P s1_e10 10000000000:1
M mn4_e10 900 4 POOL_LOG=29 $V ./ecalc 10000000000;                  P mn4_e10 10000000000:4 POOL_LOG=29
# the grids at these sizes: the plane cap lowered
R s1_e9_c26 300 DIST_LOGN_TEST=26 $V ./ecalc 1000000000;             P s1_e9_c26 1000000000:1 DIST_LOGN_TEST=26
M mn3_e9_c24 600 3 POOL_LOG=29 DIST_LOGN_TEST=24 $V ./ecalc 1000000000; P mn3_e9_c24 1000000000:3 POOL_LOG=29 DIST_LOGN_TEST=24
M mn4_e10_c27 900 4 POOL_LOG=29 DIST_LOGN_TEST=27 $V ./ecalc 10000000000; P mn4_e10_c27 10000000000:4 POOL_LOG=29 DIST_LOGN_TEST=27
# the regression gate for the C change (the extracted helpers), in the same job
[ -n "$PLAN_VALIDATE_ACCEPT" ] && { ./mnaccept.sh "$J" --only unit,e9 > "$OUT/mnaccept.log" 2>&1; echo "mnaccept rc $?: $(tail -3 "$OUT/mnaccept.log" | tr '\n' ' ')"; }
scancel "$J"; echo "done $(date)"
