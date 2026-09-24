#!/bin/bash
# G13d: run batches one after another (one job at a time), after any batch of mine still running.
#   setsid nohup bash ~/ntt-G13d/ecalc/g13d_chain.sh a1 b2 ... > ~/g13d/chain.out 2>&1 < /dev/null &
# A batch is skipped when the file ~/g13d/PAUSE exists (the three-node slot): the chain waits until it is removed.
R=~/ntt-G13d/ecalc/g13d_run.sh
for b in "$@"; do
  while pgrep -u chcoppola -f "^bash $HOME/ntt-G13d/ecalc/g13d_(run|hang).sh " > /dev/null; do sleep 10; done
  while [ -e ~/g13d/PAUSE ]; do sleep 20; done
  r=$R; case $b in h*) r=~/ntt-G13d/ecalc/g13d_hang.sh;; esac
  bash $r $b ~/g13d/$b.txt > ~/g13d/$b.out 2>&1 < /dev/null
done
