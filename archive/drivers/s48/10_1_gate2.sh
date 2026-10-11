#!/bin/bash
# OWNER=T3
# EST_MIN=6
# S48 build gate: 2 nodes, 1e9 digits, VERIFY OK + digits identical (tools/gate.sh, no build; the s48 binary built from main daeb0f9).
# Output ~/s48/gate2/gate.txt, marker ~/s48/done_gate2.
H=${SLURM_JOB_ID:?hold}
cd $HOME/ntt-wt/s48 && tools/gate.sh --nodes 2 --hold $H --line aac7 --out $HOME/s48/gate2 --timeout 600
rc=$?; echo "rc=$rc $(cat $HOME/s48/gate2/gate.txt 2>/dev/null | cut -c1-200)" > $HOME/s48/done_gate2; exit $rc
