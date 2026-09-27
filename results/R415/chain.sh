#!/bin/bash
# R4: wait for Stage 0 (~/reg15b.done with 3 lines, polled every 10 min), then the batches one after another (one job at a time)
exec >> $HOME/R415/chain.log 2>&1
echo "chain start $(date) pid $$"
while [ "$(cat $HOME/reg15b.done 2>/dev/null | wc -l)" -lt 3 ]; do sleep 600; done
echo "stage 0 done $(date)"
for b in "$@"; do echo "batch $b $(date)"; bash $HOME/R415/job.sh $b any now $HOME/R415/$b.steps; echo "batch $b end $(date)"; done
echo "chain end $(date)"
