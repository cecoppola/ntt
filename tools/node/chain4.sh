#!/bin/bash
while pgrep -f "^bash $HOME/IO15/chain3.sh" > /dev/null; do sleep 60; done
echo "batch F2 $(date)"; bash $HOME/IO15/job.sh F2 any $HOME/IO15/F2.sh; echo "batch F2 done $(date)"
