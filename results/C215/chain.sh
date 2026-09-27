#!/bin/bash
# C215: wait for the integrator's Stage 0 (~/reg15b.done with 3 lines; polled every 10 min), then 1-node jobs one after another
# until every step of ~/C215/queue.txt is in ~/C215/done.txt; the first job takes any node, every later one the same node
# (~/C215/node.lock).  usage: setsid nohup bash ~/C215/chain.sh [node] > ~/C215/chain.log 2>&1 &
C=$HOME/C215; NODE=${1:-any}; touch $C/done.txt; [ -s $C/node.lock ] && NODE=$(cat $C/node.lock)
echo "chain start $(date), node $NODE"
while [ "$(wc -l < $HOME/reg15b.done 2>/dev/null || echo 0)" -lt 3 ]; do echo "waiting for Stage 0 $(date +%T)"; sleep 600; done
echo "Stage 0 done: $(date)"
left() { grep -v '^#' $C/queue.txt | awk 'NF {print $2}' | grep -vxF -f $C/done.txt | wc -l; }
n=0
while [ $(left) -gt 0 ]; do
  n=$((n + 1)); [ $n -gt 8 ] && { echo "too many jobs; stop"; break; }
  [ -s $C/node.lock ] && NODE=$(cat $C/node.lock)
  echo "job $n on $NODE, $(left) steps left, $(date)"
  bash $C/job.sh $NODE || { echo "job.sh failed rc $?"; break; }
  [ -e $C/stop ] && { echo "stop file"; break; }
done
echo "chain end $(date), $(left) steps left"
