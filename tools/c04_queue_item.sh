#!/bin/bash
# aac7q queue item (C04, results/C04.md): measure the probe -> run relationship.  4 nodes, 5 real runs (6.441e10 digits/node, e16_headline aac7 line
# + uffd + xstats, no digit writes), each bracketed by an environment probe (tools/envprobe.sh): probe, run, probe, run, ..., probe  (6 probes shared
# between neighbours, i.e. every run has one probe before and one after; 5 x (probe, run, probe) with the shared probes counted once, to fit 30 min).
# The run's own NIC group is read the same way (envprobe.sh --snap / --group).  Output only under ~/sess1011c/c04/Q/:
#   log/rK.log (run), log/rK.probe (the 2 ENVPROBE lines bracketing it), runs.tsv, probes.tsv, summary.txt (calib_table --probe-check), C04_DONE.
# OWNER=c04
# NODES=4
# EST_MIN=30
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
set -u
STUB=${STUB:-${AQ_STUB:-0}}; J=${SLURM_JOB_ID:-}; NN=${AQ_NODES:-4}; NRUN=${NRUN:-5}; BUDGET_S=${BUDGET_S:-1620}   # no new probe/run starts after 27 min
OUT=${OUT:-$HOME/sess1011c/c04/Q}; WT=${WT:-$HOME/ntt-wt/c04}; DPN=64410000000
mkdir -p $OUT/log $OUT/probe; rm -f $OUT/C04_DONE; T0=$(date +%s)
EP=$WT/tools/envprobe.sh
if [ $STUB = 1 ]; then export STUB=1; else
  cd $WT/ecalc || exit 2; source aac7env.sh >/dev/null 2>&1
  LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh); [ -n "$LINE" ] || exit 2
  case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) echo "unexpected LINE: $LINE"; exit 2;; esac
  [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }
  ldd ./ecalc | grep -q libsma && [ -x tests/t_comm ] || { echo "ecalc without SHMEM or no tests/t_comm"; exit 2; }
fi
UFF="FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1"
left() { echo $(( BUDGET_S - ($(date +%s) - T0) )); }
evict() { [ $STUB = 1 ] && return 0; timeout 150 srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done' < /dev/null > /dev/null 2>&1; }
probe() { # label -> line in $OUT/probe/$1.line
  OUT=$OUT/probe LABEL=$1 HOLD=$J NN=$NN WT=$WT bash $EP | grep '^ENVPROBE' > $OUT/probe/$1.line; cat $OUT/probe/$1.line; }
printf 'label\tprobe_before\tprobe_after\trc\twall\ttotal\ta2a\tgroup\tfgfc\n' > $OUT/runs.tsv
evict; probe p0
for k in $(seq 1 $NRUN); do
  [ $(left) -ge 330 ] || { echo "budget: stop before r$k"; break; }
  r=r$k; evict
  bash $EP --snap $OUT/probe/$r.pre
  t1=$(date +%s)
  if [ $STUB = 1 ]; then sleep 1; { echo "  0: total   200.00 s   (bs 70.0 + 10dP 0.0 + dm 80.0 + T1 0.4 + dc 3.0 + T2 0.0 = 153.4; init 40.0; other 1.0); VmHWM 1.0 GB"; echo "comm_shmem: pe 0: all-to-all 10 exchanges, 1.0 s post to completion, 10 GB received = 12.00 GB/s per APU thread; x"; echo "mn: all 4 nodes: VERIFY OK"; } > $OUT/log/$r.log; rc=0
  else
    ( cd $WT/ecalc && exec env SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 900)) timeout 600 ./mnrun.sh $NN env $LINE ECALC_SEGV_TRACE=1 $UFF ./ecalc $((NN * DPN)) ) > $OUT/log/$r.log 2>&1 < /dev/null; rc=$?
  fi
  wall=$(( $(date +%s) - t1 )); bash $EP --snap $OUT/probe/$r.post
  read -r G F < <(bash $EP --group $OUT/probe/$r.pre $OUT/probe/$r.post)
  tot=$(sed -E 's/^ *[0-9]+: //' $OUT/log/$r.log | grep -a -m1 '^total' | awk '{print $2}')
  a2a=$(sed -E 's/^ *[0-9]+: //' $OUT/log/$r.log | grep -a 'comm_shmem: pe [0-9]*: all-to-all' | awk '!s[$3]++ {for(i=1;i<=NF;i++) if($i=="GB/s"){v+=$(i-1); n++}} END{if(n>0) printf "%.2f", v/n; else print "NA"}')
  probe p$k
  cat $OUT/probe/p$((k-1)).line $OUT/probe/p$k.line > $OUT/log/$r.probe; echo "ENVPROBE_RUNGROUP group=$G fgfc=$F" >> $OUT/log/$r.probe
  printf '%s\tp%d\tp%d\t%s\t%s\t%s\t%s\t%s\t%s\n' $r $((k-1)) $k $rc $wall "${tot:-NA}" "$a2a" "${G:-NA}" "${F:-NA}" >> $OUT/runs.tsv
  echo "$r: rc $rc wall ${wall}s total ${tot:-NA} a2a $a2a run-group ${G:-NA}"
done
{ echo "runs.tsv:"; cat $OUT/runs.tsv; echo; for f in $OUT/probe/p*.line; do echo "$(basename $f .line): $(cat $f)"; done; echo
  python3 -I $WT/ecalc/calib_table.py $OUT/log --probe-check 2>&1 | head -20; } > $OUT/summary.txt
[ $STUB = 1 ] || squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /'
echo "SUCCESS $(date +%s)" > $OUT/C04_DONE; cat $OUT/summary.txt | head -30
