#!/bin/bash
# O2 aac6 driver (results/O2.md): waits for 03:31 Eastern, then on one PPAC SPX node (s24-26; H4 takes the other from 03:35):
#   tools/gate.sh --build --nodes 1 --line none (switch off) and --env DBIG_SMALL_BOUNCE=1 (1e9, digits identical),
#   tests/t_dbig with DBIG_SMALL_BOUNCE=1, tests/t_bounce off/on x5 alternating (the cold first copy of 27302 limbs, as one
#   node's share in the 4-node reciprocal gather), and t_bounce at 109206 limbs (the whole T) x2 per arm.
# Clone ~/ntt-o2 (from ~/ntt + bundle ~/o2/o2.bundle), output ~/o2/, done-marker ~/o2/O2_AAC6_DONE, summary ~/o2/summary.txt.
# Start: setsid nohup bash -l ~/o2/o2_aac6.sh > ~/o2/driver.log 2>&1 < /dev/null &      (STOP: touch ~/o2/STOP before the start time)
set -u
O=$HOME/o2; C=$HOME/ntt-o2; S=$O/summary.txt; NODE=${O2_NODE:-aac6-spx-b}
START=${O2_START:-$(TZ=America/New_York date -d "2026-10-12 03:31" +%s)}
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $S; }
mkdir -p $O; : > $S
say "o2 aac6 driver; start at $(TZ=America/New_York date -d @$START '+%F %H:%M ET') on $NODE"
while [ $(date +%s) -lt $START ]; do [ -e $O/STOP ] && { say "STOP"; touch $O/O2_AAC6_DONE; exit 0; }; s=$(( START - $(date +%s) )); sleep $(( s > 300 ? 300 : s )); done
module load rocm >/dev/null 2>&1
rm -rf $C; git clone -q $HOME/ntt $C && git -C $C fetch -q $HOME/o2/o2.bundle o2:o2 && git -C $C checkout -q o2 || { say "clone failed"; touch $O/O2_AAC6_DONE; exit 1; }
say "clone $(git -C $C log --oneline -1 | cut -c1-70)"
J=$(sbatch -p PPAC_MI300A_SPX -N1 -w $NODE --gpus=4 -t 0:40:00 -J o2 --parsable --wrap "sleep 2400" < /dev/null 2>>$O/sbatch.err); J=${J%%;*}
[ -n "$J" ] || { say "sbatch failed: $(tail -1 $O/sbatch.err)"; touch $O/O2_AAC6_DONE; exit 1; }
w=0; while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do sleep 15; w=$((w + 15)); [ $w -ge 600 ] && { scancel $J; say "hold $J pending 10 min, cancelled"; touch $O/O2_AAC6_DONE; exit 1; }; done
say "hold $J on $(squeue -j $J -h -o %N)"
T0=$(date +%s); cd $C/ecalc
bash $C/tools/gate.sh --build --nodes 1 --hold $J --line none --out $O/gate_off --timeout 900 < /dev/null > $O/gate_off.out 2>&1; say "gate_off rc=$? $(cut -c1-180 $O/gate_off/gate.txt 2>/dev/null)"
bash $C/tools/gate.sh --nodes 1 --hold $J --line none --env DBIG_SMALL_BOUNCE=1 --out $O/gate_on --timeout 900 < /dev/null > $O/gate_on.out 2>&1; say "gate_on rc=$? $(cut -c1-180 $O/gate_on/gate.txt 2>/dev/null)"
make -s -j16 tests/t_dbig tests/t_bounce > $O/build_tests.log 2>&1; say "tests build rc=$?"
R1="srun --jobid=$J -N1 -n1 --gpus-per-node=4 --overlap --export=ALL"
timeout 900 $R1 env DBIG_SMALL_BOUNCE=1 tests/t_dbig < /dev/null > $O/t_dbig_on.log 2>&1; say "t_dbig on rc=$? $(grep -a VERIFY $O/t_dbig_on.log | tail -1)"
: > $O/unit.txt
for i in 1 2 3 4 5; do for sw in 0 1; do [ $(( $(date +%s) - T0 )) -lt 2000 ] || break 2
  timeout 120 $R1 env DBIG_SMALL_BOUNCE=$sw DBIG_BOUNCE_TS=1 tests/t_bounce 27302 < /dev/null 2>&1 | grep -a "BOUNCE\|bounce_init\|VERIFY" >> $O/unit.txt; done; done
for i in 1 2; do for sw in 0 1; do [ $(( $(date +%s) - T0 )) -lt 2100 ] || break 2
  timeout 120 $R1 env DBIG_SMALL_BOUNCE=$sw DBIG_BOUNCE_TS=1 tests/t_bounce 109206 < /dev/null 2>&1 | grep -a "BOUNCE\|bounce_init\|VERIFY" >> $O/unit.txt; done; done
grep -a "^BOUNCE" $O/unit.txt | tee -a $S >/dev/null
scancel $J; sleep 5; say "hold $J cancelled; squeue: $(squeue -j $J -h | wc -l) lines"
say "DONE"; touch $O/O2_AAC6_DONE
