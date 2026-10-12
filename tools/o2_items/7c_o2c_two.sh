#!/bin/bash
# aac7q queue item (O2c, results/O2.md on branch o2): the DBIG_SMALL_BOUNCE ABBA of items 70/72 repeated at 2 nodes (item 70: 4 nodes, total -30.08 s; item 72: 6 nodes, -4.6 s ns).
# Gives the fix's total effect at 2 nodes.  Reuses item 70's binary (~/o2/ecalc_o2, SHMEM), worktree (~/ntt-wt/o2: LINE from ecalc/e16_headline.sh, tools/o2_stats.py, tools/envprobe.sh)
# and the logic of item 72: 2 nodes x 6.441e10 digits per node, aac7 line, no digit writes, print-only COMM_HS_TS=1 NEWTON_MN_RECIP_TS=2 DBIG_BOUNCE_TS=1 in both arms;
# paired AB/BA rounds, A = off (default), B = DBIG_SMALL_BOUNCE=1; stop rule: >= 4 pairs and p < 0.05 on the chain interval, or 6 pairs, or no run starts after 55 min.
# Per node MemFree/Cached snapshot and the NIC launch group (envprobe --snap/--group) per run.  Own output dir and marker, nothing shared with other items:
# Output ~/o2c/: log/ mem.tsv groups.tsv summary.txt stats.txt; done-marker ~/o2c/DONE.  STUB=1: dry run.
# OWNER=o2
# NODES=2
# EST_MIN=60
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
# ALLOW_SLOW=0
set -u
WT=$HOME/ntt-wt/o2; E=$WT/ecalc; O=$HOME/o2c; NN=2; DPN=64410000000; EC=$HOME/o2/ecalc_o2; EP=$WT/tools/envprobe.sh
mkdir -p $O/log; rm -f $O/DONE; cd $E || exit 2
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh)
case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) echo "unexpected LINE: $LINE"; exit 2;; esac
if [ "${STUB:-0}" = 1 ]; then
  echo "STUB ok: rev $(git rev-parse --short HEAD); LINE=${LINE:0:60}..."; bash -n $0 && echo syntax ok
  python3 $WT/tools/o2_stats.py $O/log >/dev/null && echo stats ok
  [ -x $EC ] && ldd $EC | grep -q libsma && strings $EC | grep -q DBIG_SMALL_BOUNCE && echo "binary ok (SHMEM, switch present)"
  [ -f $EP ] && bash -n $EP && bash $EP --group /dev/null /dev/null && echo "envprobe ok"; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s); S=$O/summary.txt; : > $S
el() { echo $(( $(date +%s) - T0 )); }
source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
echo "start $(date) hold $J rev $(git rev-parse --short HEAD) binary $(md5sum < $EC | cut -c1-8) hosts $(squeue -j $J -h -o %N)" >> $S
ldd $EC | grep -q libsma || { echo "binary without SHMEM" >> $S; touch $O/DONE; exit 2; }
X="ECALC_SEGV_TRACE=1 FI_MR_CACHE_MONITOR=userfaultfd NEWTON_MN_RECIP_TS=2 COMM_HS_TS=1 DBIG_BOUNCE_TS=1"
: > $O/mem.tsv; printf 'run\tgroup\tfgfc_M\n' > $O/groups.tsv
one() { # pair arm
  local k=$1 arm=$2 aw=""; [ $arm = B ] && aw=DBIG_SMALL_BOUNCE=1
  [ $(el) -lt 3300 ] || { echo "skip p${k}_$arm (time)" >> $S; return 1; }
  srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 -c1 timeout 100 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; echo "$(hostname) $(grep -E "^(MemFree|Cached):" /proc/meminfo | tr -s " " | tr "\n" " ")"' < /dev/null 2>/dev/null | sed "s/^/p${k}_$arm /" >> $O/mem.tsv
  local lab=o2c_p${k}_$arm t1=$(date +%s)
  SLURM_JOB_ID=$J bash $EP --snap $O/$lab.pre > /dev/null 2>&1
  SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 420 ./mnrun.sh $NN env $LINE $X $aw $EC $((NN * DPN)) < /dev/null > $O/log/$lab.log 2>&1; local rc=$?
  SLURM_JOB_ID=$J bash $EP --snap $O/$lab.post > /dev/null 2>&1
  local grp=$(bash $EP --group $O/$lab.pre $O/$lab.post 2>&1 | tail -1); rm -f $O/$lab.pre $O/$lab.post
  printf '%s\t%s\n' $lab "$grp" >> $O/groups.tsv
  echo "$lab rc=$rc wall=$(( $(date +%s) - t1 )) el=$(el) nicgroup/fgfcM=[$grp] $(grep -a -c "all $NN nodes: VERIFY OK" $O/log/$lab.log)verify $(grep -a -m1 ': total ' $O/log/$lab.log | cut -c1-70) $(grep -a -m1 'single-node chain' $O/log/$lab.log | grep -o '([0-9.]* s)')" >> $S
  return 0; }
for k in 1 2 3 4 5 6; do
  if [ $((k % 2)) = 1 ]; then one $k A && one $k B || break; else one $k B && one $k A || break; fi
  if [ $k -ge 4 ] && python3 $WT/tools/o2_stats.py $O/log --stop > /dev/null 2>&1; then echo "stop rule met after pair $k" >> $S; break; fi
done
python3 $WT/tools/o2_stats.py $O/log > $O/stats.txt 2>&1; cat $O/stats.txt >> $S
{ echo "NIC launch group per run (A = slow, B = fast; fgfc in millions):"; cat $O/groups.tsv; } >> $S
nruns=$(ls $O/log/o2c_p*_?.log 2>/dev/null | wc -l); nok=$(grep -l "all $NN nodes: VERIFY OK" $O/log/o2c_p*_?.log 2>/dev/null | wc -l)
echo "runs=$nruns verify_ok=$nok" >> $S
squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 >> $S
echo "DONE $(date) el=$(el)" >> $S; touch $O/DONE; exit 0
