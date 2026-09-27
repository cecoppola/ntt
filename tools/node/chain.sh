#!/bin/bash
# IO15: the batches one after another (one job at a time); waits for the integrator's Stage 0 (~/reg15b.done: 3 lines),
# polling every 10 min.  usage: setsid nohup bash ~/IO15/chain.sh <node|any> A B ... > ~/IO15/chain.log 2>&1 &
NODE=$1; shift
while [ "$(wc -l < $HOME/reg15b.done 2>/dev/null || echo 0)" -lt 3 ]; do echo "waiting for Stage 0 $(date +%T)"; sleep 600; done
for b in "$@"; do echo "batch $b $(date)"; bash $HOME/IO15/job.sh $b $NODE $HOME/IO15/$b.sh; echo "batch $b done $(date)"; done
