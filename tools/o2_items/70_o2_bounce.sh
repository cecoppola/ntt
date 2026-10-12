#!/bin/bash
# aac7q queue item (O2, TASKS 35, results/O2.md on branch o2): the reciprocal-gather stall and DBIG_SMALL_BOUNCE.
#   1. tools/gate.sh --build --nodes 2 (switch off, the default) and tools/gate.sh --nodes 2 --env DBIG_SMALL_BOUNCE=1 (1e9, digits identical)
#   2. 1 node: tests/t_dbig with DBIG_SMALL_BOUNCE=1 (small + large paths), tests/t_bounce off/on x3 (the cold first copy)
#   3. 4 nodes x 6.441e10, aac7 line, no digit writes, print-only COMM_HS_TS=1 NEWTON_MN_RECIP_TS=2 DBIG_BOUNCE_TS=1 in both arms;
#      paired AB/BA rounds, A = off, B = DBIG_SMALL_BOUNCE=1; stop rule: >= 4 pairs and p < 0.05 on the chain interval, or 6 pairs,
#      or no run starts after 50 min.  Per-node MemFree/Cached snapshot before every run.
#   4. If clean (both gates PASS, t_dbig OK, every timing run VERIFY OK, chain mean B-A < -1 s): installs ~/aac7q/queue/90_o2_c45.sh.
# Output ~/o2/: gate_off/ gate_on/ unit.txt log/ mem.tsv summary.txt stats.txt, done-marker ~/o2/O2_DONE.
# OWNER=o2
# NODES=4
# EST_MIN=60
# APPROVED=coordinator-2026-10-11
# EXCLUSIVE=0
# ALLOW_SLOW=0
set -u
WT=$HOME/ntt-wt/o2; E=$WT/ecalc; O=$HOME/o2; NN=4; DPN=64410000000
mkdir -p $O/log; cd $E || exit 2
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh)
case "$LINE" in *MN_T_CHUNK_MB=2048*COMM_LAYER_INTER2=1*COMM_LAYER_VSLOT_POOL=1*) ;; *) echo "unexpected LINE: $LINE"; exit 2;; esac
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: rev $(git rev-parse --short HEAD); LINE=${LINE:0:60}..."; bash -n $0 && echo syntax ok; python3 $WT/tools/o2_stats.py $O/log >/dev/null && echo stats ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s); S=$O/summary.txt; : > $S
el() { echo $(( $(date +%s) - T0 )); }
source aac7env.sh >/dev/null 2>&1; module load cray-dsmml cray-openshmemx >/dev/null 2>&1
echo "start $(date) hold $J rev $(git rev-parse --short HEAD) hosts $(squeue -j $J -h -o %N)" >> $S
# 1. gates
bash $WT/tools/gate.sh --build --nodes 2 --hold $J --out $O/gate_off --timeout 900 < /dev/null > $O/gate_off.out 2>&1; g1=$?
echo "gate_off rc=$g1 el=$(el): $(cut -c1-200 $O/gate_off/gate.txt 2>/dev/null)" >> $S
make SHMEM_CRAY=1 GMP_HOME=~/gmp tests/t_dbig tests/t_bounce > $O/build_tests.log 2>&1; echo "tests build rc=$? el=$(el)" >> $S
bash $WT/tools/gate.sh --nodes 2 --hold $J --env DBIG_SMALL_BOUNCE=1 --out $O/gate_on --timeout 900 < /dev/null > $O/gate_on.out 2>&1; g2=$?
echo "gate_on rc=$g2 el=$(el): $(cut -c1-200 $O/gate_on/gate.txt 2>/dev/null)" >> $S
# 2. one node
R1="srun --jobid=$J -N1 -n1 --gpus=4 -c 192 --overlap --mem=0"
timeout 600 $R1 env DBIG_SMALL_BOUNCE=1 tests/t_dbig < /dev/null > $O/t_dbig_on.log 2>&1; u1=$?
grep -q "VERIFY OK" $O/t_dbig_on.log || u1=1; echo "t_dbig DBIG_SMALL_BOUNCE=1 rc=$u1 $(grep -a 'VERIFY' $O/t_dbig_on.log | tail -1)" >> $S
: > $O/unit.txt
for i in 1 2 3; do for sw in 0 1; do timeout 120 $R1 env DBIG_SMALL_BOUNCE=$sw DBIG_BOUNCE_TS=1 tests/t_bounce < /dev/null 2>&1 | grep -a "BOUNCE\|bounce_init\|VERIFY" >> $O/unit.txt; done; done
grep -a "^BOUNCE" $O/unit.txt >> $S; echo "unit el=$(el)" >> $S
# 3. timing
EC=$O/ecalc_o2; cp -f ecalc $EC.tmp && mv -f $EC.tmp $EC; ldd $EC | grep -q libsma || { echo "binary without SHMEM" >> $S; touch $O/O2_DONE; exit 2; }
X="ECALC_SEGV_TRACE=1 FI_MR_CACHE_MONITOR=userfaultfd NEWTON_MN_RECIP_TS=2 COMM_HS_TS=1 DBIG_BOUNCE_TS=1"
: > $O/mem.tsv
one() { # pair arm
  local k=$1 arm=$2 aw=""; [ $arm = B ] && aw=DBIG_SMALL_BOUNCE=1
  [ $(el) -lt 3000 ] || { echo "skip p${k}_$arm (time)" >> $S; return 1; }
  srun --jobid=$J -N$NN -n$NN --ntasks-per-node=1 --overlap --mem=0 -c1 timeout 100 bash -c 'for f in $HOME/ref/e_*; do dd if=$f iflag=nocache count=0 status=none 2>/dev/null; done; echo "$(hostname) $(grep -E "^(MemFree|Cached):" /proc/meminfo | tr -s " " | tr "\n" " ")"' < /dev/null 2>/dev/null | sed "s/^/p${k}_$arm /" >> $O/mem.tsv
  local lab=o2_p${k}_$arm t1=$(date +%s)
  SLURM_JOB_ID=$J MNRUN_NODES=$NN MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout 420 ./mnrun.sh $NN env $LINE $X $aw $EC $((NN * DPN)) < /dev/null > $O/log/$lab.log 2>&1; local rc=$?
  echo "$lab rc=$rc wall=$(( $(date +%s) - t1 )) el=$(el) $(grep -a -c 'all 4 nodes: VERIFY OK' $O/log/$lab.log)verify $(grep -a -m1 ': total ' $O/log/$lab.log | cut -c1-70) $(grep -a -m1 'single-node chain' $O/log/$lab.log | grep -o '([0-9.]* s)')" >> $S
  return 0; }
for k in 1 2 3 4 5 6; do
  if [ $((k % 2)) = 1 ]; then one $k A && one $k B || break; else one $k B && one $k A || break; fi
  if [ $k -ge 4 ] && python3 $WT/tools/o2_stats.py $O/log --stop > /dev/null 2>&1; then echo "stop rule met after pair $k" >> $S; break; fi
done
python3 $WT/tools/o2_stats.py $O/log > $O/stats.txt 2>&1; cat $O/stats.txt >> $S
# 4. follow-up item
nruns=$(ls $O/log/o2_p*_?.log 2>/dev/null | wc -l); nok=$(grep -l 'all 4 nodes: VERIFY OK' $O/log/o2_p*_?.log 2>/dev/null | wc -l)
cm=$(grep -a '^paired B-A chain' $O/stats.txt | sed -E 's/.*mean ([-+0-9.]+) s.*/\1/')
if [ $g1 = 0 ] && [ $g2 = 0 ] && [ $u1 = 0 ] && [ $nruns -ge 4 ] && [ $nruns = $nok ] && [ -n "$cm" ] && python3 -c "import sys; sys.exit(0 if float('$cm') < -1.0 else 1)"; then
  [ -f $O/90_o2_c45.sh ] && cp $O/90_o2_c45.sh $HOME/aac7q/queue/.90_o2_c45.tmp && mv $HOME/aac7q/queue/.90_o2_c45.tmp $HOME/aac7q/queue/90_o2_c45.sh && echo "installed 90_o2_c45.sh" >> $S
else echo "follow-up NOT installed (g_off=$g1 g_on=$g2 t_dbig=$u1 runs=$nruns ok=$nok chain_mean=$cm)" >> $S; fi
squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 >> $S
echo "DONE $(date) el=$(el)" >> $S; touch $O/O2_DONE; exit 0
