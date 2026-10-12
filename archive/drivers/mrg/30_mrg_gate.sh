#!/bin/bash
# aac7q queue item MRG (2026-10-12): the merge gate of branch mrg (h4 + cand + o3 + o8 + g6; NEW defaults DBIG_SMALL_BOUNCE=1,
#   DIST_GATHER_RAW=2, BS_SEED_PULL=1 built in, nothing set for them on the command line).
#   1. tools/gate.sh --build --nodes 2 (aac7 LINE, SHMEM build in the worktree ~/ntt-wt/mrg, -DCOMM_SHMEM checked): 1e9 identical.
#   2. one 2-node run at 6.441e10 digits per node (1.2882e11), aac7 LINE, no digit writes: "all 2 nodes: VERIFY OK".
#   Own output ~/mrg7/ only; marker ~/mrg7/DONE (SUCCESS|FAILED_<step>).
# OWNER=mrg
# NODES=2
# EST_MIN=30
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
# ALLOW_SLOW=0
set -u
WT=$HOME/ntt-wt/mrg; E=$WT/ecalc; O=$HOME/mrg7; NN=2; DPN=64410000000
mkdir -p $O; rm -f $O/DONE; cd $E || exit 2
J=$SLURM_JOB_ID; T0=$(date +%s); S=$O/summary.txt; : > $S
el() { echo $(( $(date +%s) - T0 )); }
fin() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' >> $S; echo "DONE $1 el=$(el)" >> $S; echo "$1 $(date +%s)" > $O/DONE; }
trap 'echo "item interrupted" >> $S; fin INTERRUPTED; exit 1' INT TERM
grep -q 'atoi(getenv("BS_SEED_PULL")) : 1' dbig.c && grep -q 'atoi(getenv("DBIG_SMALL_BOUNCE")) : 1' dbig.c && grep -q 'v = e ? atoi(e) : 2; }' rns_dist.c || { echo "tree without the new defaults" >> $S; fin FAILED_tree; exit 1; }
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh)
case "$LINE" in *DBIG_SMALL_BOUNCE*|*DIST_GATHER_RAW*|*BS_SEED_PULL*) echo "LINE sets one of the three switches: $LINE" >> $S; fin FAILED_line; exit 1;; esac
source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
echo "start $(date) rev $(git rev-parse --short HEAD) nodes $NN" >> $S
bash $WT/tools/gate.sh --build --nodes 2 --hold $J --out $O/gate --timeout 900 < /dev/null > $O/gate.out 2>&1; grc=$?
echo "gate rc=$grc el=$(el): $(cut -c1-300 $O/gate/gate.txt 2>/dev/null)" >> $S
grep -q -- -DCOMM_SHMEM $O/gate/build.log 2>/dev/null && echo "build log has -DCOMM_SHMEM" >> $S || { echo "build log without -DCOMM_SHMEM" >> $S; grc=1; }
[ $grc = 0 ] || { fin FAILED_gate; exit 1; }
log=$O/run_2n.log; t1=$(date +%s)
SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 600 ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 FI_MR_CACHE_MONITOR=userfaultfd ECALC_INIT_TL=1 $E/ecalc $((NN * DPN)) < /dev/null > $log 2>&1; rc=$?
V=none; grep -aq "all $NN nodes: VERIFY OK" $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED
TOT=$(grep -a -m1 -E '^ *0: total ' $log | awk '{print $3}')
echo "run 2 nodes x 6.441e10: rc=$rc verify=$V total=${TOT:-NA} wall=$(( $(date +%s) - t1 )) s; pull: $(grep -a -m1 'seed pull kernels' $log | cut -c1-90)" >> $S
grep -a -m1 'config:' $log | cut -c1-200 >> $S
[ $rc = 0 ] && [ $V = ok ] || { fin FAILED_run; exit 1; }
fin SUCCESS; exit 0
