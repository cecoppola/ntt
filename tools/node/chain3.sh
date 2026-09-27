#!/bin/bash
# IO15: the third chain (2026-09-27): F (1 node: gates, size 4), then W (2 nodes).
echo "batch F $(date)"; bash $HOME/IO15/job.sh F any $HOME/IO15/F.sh; echo "batch F done $(date)"
echo "batch W $(date)"; NN=2 TMAX=1500 bash $HOME/IO15/job.sh W2 any $HOME/IO15/W.sh; echo "batch W done $(date)"
