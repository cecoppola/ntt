#!/bin/bash
# s44_node.sh <jobid> <tag>: the three S44 ABBAs on one node, then cancels its own job.  Outputs in ~/s44out/<tag>_*.
J=$1; T=$2; D=$HOME/ntt-s44/tools; O=$HOME/s44out
$D/s44_abba.sh $J $O/${T}_b "X=1" "DBIG_MAXIDX_TOP=1"
$D/s44_abba.sh $J $O/${T}_c "DBIG_MAXIDX_TOP=1" "DBIG_MAXIDX_TOP=1 DBIG_QSEL=1"
$D/s44_abba.sh $J $O/${T}_a "DBIG_ADDSUB2=0" "X=1"
echo done > $O/${T}_NODEDONE
scancel $J
