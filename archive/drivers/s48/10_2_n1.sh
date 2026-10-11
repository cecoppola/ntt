#!/bin/bash
# OWNER=T3
# EST_MIN=8
# S48 scaling point: 1 node(s), 2 timed runs at 6.441e10 digits/node, aac7 line + COMM_XSTATS MN_WAIT_STATS ECALC_INIT_TL, no digit output.
# Outputs ~/s48/ (log/n1_r*.log, summary.tsv, done_n1).  Run as: SLURM_JOB_ID=<hold> bash item.sh   (DRY=1 S48_OUT=<dir> = stub)
exec bash $HOME/s48/s48_run.sh 1 2
