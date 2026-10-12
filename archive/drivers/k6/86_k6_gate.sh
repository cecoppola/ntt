#!/bin/bash
# aac7q queue item (results/Q1.md): K6 2-node gate of RNS_DIST_OVERLAP=1 (branch k6; the node-local products of a multi-node run take the switch).
# OWNER=k6
# NODES=2
# EST_MIN=20
# APPROVED=coordinator/2026-10-12
# NOT_BEFORE=
# NOT_AFTER=
# EXCLUSIVE=0
# ALLOW_SLOW=0
#
# Steps use --overlap --mem=0 through gate.sh/mnrun.sh (no -w).  Output under ~/k6/ (gate2: build.log, run.log, gate.txt; the digits are deleted by gate.sh).
set -u
J=$SLURM_JOB_ID; O=$HOME/k6; mkdir -p $O
cd ~/ntt-acc || exit 2
git fetch -q origin k6 || { echo "fetch k6 failed" | tee $O/gate2.txt; exit 2; }
[ -d ~/ntt-wt/k6 ] || git worktree add -q ~/ntt-wt/k6 origin/k6 || { echo "worktree failed" | tee $O/gate2.txt; exit 2; }
git -C ~/ntt-wt/k6 checkout -q --detach origin/k6 || true
cd ~/ntt-wt/k6/ecalc && source aac7env.sh >/dev/null 2>&1
SLURM_JOB_ID=$J bash ~/ntt-wt/k6/tools/gate.sh --build --nodes 2 --hold $J --line aac7 --env "RNS_DIST_OVERLAP=1" --out $O/gate2 --timeout 900 > $O/gate2.log 2>&1; rc=$?
cp $O/gate2/gate.txt $O/gate2.txt 2>/dev/null || echo "gate FAIL | no summary" > $O/gate2.txt
cat $O/gate2.txt
echo "K6 2-node gate rc $rc: $(cat $O/gate2.txt)" > $O/GATE2_DONE
exit $rc
