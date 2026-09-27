#!/bin/bash
# C215 (Phase 15, agent C2): one unattended 1-node job that consumes the step queue ~/C215/queue.txt (lines not yet in
# ~/C215/done.txt) until the next step would not fit the 45-min job.  Adapted from V2's ~/V214/job.sh.
#   usage: job.sh <node|any>          prints the node it ran on to ~/C215/node.lock (the chain pins every later job to it)
# steps: "file <tag> <digits> [VAR=val ...]"   ecalc <digits> /tmp/c2_<tag>.txt, then sha1 of the file on the node (dd iflag=direct)
#        "nofile <tag> <digits> [VAR=val ...]" ecalc <digits> (no digit file: the wall without the write)
# every run: ECALC_VERBOSE=2 RNS_VERBOSE=1; the process wall measured inside srun (date before / after ecalc); the code checked.
NODE=$1; W=; [ "$NODE" = any ] || W="-w $NODE"
C=$HOME/C215; R=$HOME/ntt-C215; LOG=$C/logs; mkdir -p $LOG; touch $C/done.txt
exec >> $C/driver.log 2>&1
BASE=0b80ca3                                   # int15 = B0: the ecalc tree must be this commit's
if [ "$(git -C $R rev-parse HEAD:ecalc)" != "$(git -C $R rev-parse $BASE:ecalc)" ]; then echo "WRONG COMMIT: ecalc tree differs from $BASE"; exit 1; fi
[ $R/ecalc/ecalc -nt $R/ecalc/rns_dist.c ] || { echo "WRONG COMMIT: ecalc binary older than the source"; exit 1; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 $W --gpus=4 -t 0:45:00 -J C2 --parsable --wrap "sleep 2700")
echo "== job $J node $NODE submitted $(date)"
trap 'scancel $J 2>/dev/null' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do [ -z "$(squeue -j $J -h -o %T)" ] && { echo "job $J gone"; exit 1; }; sleep 15; done
T0=$(date +%s); HN=$(squeue -j $J -h -o %N); echo "running $(date) on $HN; commit $(git -C $R log --oneline -1)"; echo $HN > $C/node.lock
N() { srun --jobid=$J -N1 --overlap bash -c "$*" < /dev/null; }
N "hostname; df -h /tmp | tail -1; free -g | head -2; rm -rf /tmp/c2_*" > $LOG/node_$J.txt 2>&1
est() { case "$1:$2" in file:1300*) echo 540;; file:1000*) echo 420;; file:*) echo 170;; nofile:*) echo 290;; *) echo 400;; esac; }
while read -r kind tag dg rest; do
  [ -z "$kind" ] && continue; case "$kind" in \#*) continue;; esac
  grep -qx "$tag" $C/done.txt && continue
  el=$(( $(date +%s) - T0 )); e=$(est $kind $dg)
  [ $(( el + e )) -gt 2520 ] && { echo "stop before $tag (elapsed $el s + est $e s)"; break; }
  echo "== $kind $tag $dg $rest  $(date +%T) (job $J, $HN)"
  OUT=-; [ $kind = file ] && OUT=/tmp/c2_$tag.txt
  timeout 1200 srun --jobid=$J -N1 --gpus=4 --overlap bash $C/run1.sh $dg $OUT $rest > $LOG/$tag.log 2>&1 < /dev/null
  echo "   rc $?; $(grep -a '^C2WALL' $LOG/$tag.log); $(grep -a '^total' $LOG/$tag.log | tail -1 | cut -c1-120); $(grep -a 'VERIFY' $LOG/$tag.log | tail -1)"
  if [ $kind = file ]; then
    N "ls -la $OUT; du -sh $OUT.top 2>/dev/null; t0=\$(date +%s); dd if=$OUT iflag=direct bs=64M status=none | sha1sum; echo \"hash \$(( \$(date +%s) - t0 )) s\"; rm -rf $OUT $OUT.*" > $LOG/$tag.sha 2>&1
    echo "   sha $(grep -- ' -$' $LOG/$tag.sha)"
  fi
  echo $tag >> $C/done.txt
done < $C/queue.txt
N "rm -rf /tmp/c2_*; ls /tmp | grep -c c2_" > /dev/null 2>&1
echo "== job $J done $(date), $(( $(date +%s) - T0 )) s"
