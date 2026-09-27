#!/bin/bash
# IO15: the second chain -- after the first (chain.sh) ends: Eck (V2's checkpoint item D), D (1e11 packed to NFS), W (2 nodes).
while pgrep -f "^bash $HOME/IO15/chain.sh" > /dev/null; do sleep 120; done
for b in Eck D; do echo "batch $b $(date)"; bash $HOME/IO15/job.sh $b any $HOME/IO15/$b.sh; echo "batch $b done $(date)"; done
echo "batch W $(date)"; NN=2 TMAX=1200 bash $HOME/IO15/job.sh W any $HOME/IO15/W.sh; echo "batch W done $(date)"
