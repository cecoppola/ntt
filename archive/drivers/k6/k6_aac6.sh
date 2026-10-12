#!/bin/bash
# k6_aac6.sh (K6) - aac6 fire-and-forget driver, run on the aac6 login node:  setsid nohup bash ~/ntt-k6/archive/drivers/k6/k6_aac6.sh > ~/k6/driver.out 2>&1 < /dev/null &
#   Slots of <= 44 min on PPAC_MI300A_SPX (no -w: either 4-APU node; the job queues behind the others), each released at its end.
#   Slot 1 (stage, ~15 min): build (module rocm), tools/gate.sh 1e9 with RNS_DIST_OVERLAP=1 (digits vs the reference), the same with
#     RNS_STRATEGY=C (every product in the C form, so the overlap path runs at every size), tests/t_strategy V with the switch (the
#     overlapped C form against the B forms, 6 shapes), and an on/off digit comparison at 4e9 digits under RNS_STRATEGY=C (cmp of the two
#     output files, then deleted).  Then ABBA at 1e10 digits (stop: p<0.05 after >= 3 rounds, CI half-width <= 0.3 s, or 8 rounds).
#   Later slots: ABBA at 6.441e10 digits (CI half-width <= 1.0 s), resuming from ppac/abba64/pairs.tsv until the rule is met or D.
#   env: STUB=1 (logic only), WT (~/ntt-k6), O (~/k6), D (deadline epoch, default now + 12 h), MAXR (8), SLOT_MIN (44)
# Markers: $O/K6_DONE (SUCCESS / PARTIAL / FAILED + reason), $O/summary.txt, results in $O/stage/, $O/abba10/, $O/abba64/.  Jobs: only ours (-J k6), cancelled by id.
STUB=${STUB:-0}; WT=${WT:-$HOME/ntt-k6}; E=$WT/ecalc; O=${O:-$HOME/k6}; D=${D:-$(( $(date +%s) + 12 * 3600 ))}; MAXR=${MAXR:-8}; SLOT_MIN=${SLOT_MIN:-44}
mkdir -p $O/stage; say() { echo "$(TZ=America/New_York date '+%F %T %Z') $1" | tee -a $O/summary.txt; }
hold() {   # minutes -> prints the job id once RUNNING (rc 0); pending beyond 90 min or gone -> cancelled, rc 1
  if [ "$STUB" = 1 ]; then echo 0; return 0; fi
  local J st i; J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:$1:00 -J k6 --parsable --wrap "sleep $(( $1 * 60 ))" < /dev/null 2>>$O/sbatch.err); J=${J%%;*}; [ -n "$J" ] || return 1
  for i in $(seq 1 540); do st=$(squeue -j $J -h -o %T); [ "$st" = RUNNING ] && { echo $J; return 0; }; [ -z "$st" ] && return 1; [ $(date +%s) -gt $D ] && break; sleep 10; done
  scancel $J; return 1
}
release() { [ "$STUB" = 1 ] || { [ "$1" != 0 ] && scancel $1 2>/dev/null; }; [ "$STUB" = 1 ] || [ "$(squeue -s -j $1 -h 2>/dev/null | wc -l)" = 0 ] || say "WARNING: steps left in $1"; }
RS() {   # hold env-words cmd...  (srun into our hold; 15-min cap)
  local H=$1 XE=$2; shift 2
  if [ "$STUB" = 1 ]; then echo "STUB srun: env $XE $*"; else timeout 900 srun --jobid=$H --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; env $XE $*" < /dev/null; fi
}
stage() {   # hold -> $O/stage/STAGE (PASS/FAIL lines)
  local H=$1 P=$O/stage rc; : > $P/STAGE
  if [ "$STUB" = 1 ]; then echo "gate PASS | STUB" > $P/gate_on.txt; cp $P/gate_on.txt $P/gate_onC.txt; echo "t_strategy rc 0" > $P/t_strategy.log; echo "cmp4e9 PASS | STUB" >> $P/STAGE; fi
  if [ "$STUB" != 1 ]; then
  [ -x $WT/tools/unpack_digits ] || bash -lc "cd $WT/tools && make -s unpack_digits" > $P/build_tools.log 2>&1
  timeout 1500 bash -lc "module load rocm >/dev/null 2>&1; bash $WT/tools/gate.sh --build --nodes 1 --hold $H --line none --env 'RNS_DIST_OVERLAP=1' --out $P/g_on --timeout 600" < /dev/null > $P/gate_on.log 2>&1
  cp $P/g_on/gate.txt $P/gate_on.txt 2>/dev/null || echo "gate FAIL | no summary (on)" > $P/gate_on.txt
  timeout 900 bash -lc "module load rocm >/dev/null 2>&1; bash $WT/tools/gate.sh --nodes 1 --hold $H --line none --env 'RNS_DIST_OVERLAP=1 RNS_STRATEGY=C' --out $P/g_onC --timeout 600" < /dev/null > $P/gate_onC.log 2>&1
  cp $P/g_onC/gate.txt $P/gate_onC.txt 2>/dev/null || echo "gate FAIL | no summary (on, C)" > $P/gate_onC.txt
  RS $H "RNS_DIST_OVERLAP=1" "./tests/t_strategy 27 1 V" > $P/t_strategy.log 2>&1; echo "t_strategy rc $?" >> $P/t_strategy.log
  RS $H "RNS_STRATEGY=C" "./ecalc 4000000000 $P/off.out" > $P/cmp_off.log 2>&1
  RS $H "RNS_STRATEGY=C RNS_DIST_OVERLAP=1" "./ecalc 4000000000 $P/on.out" > $P/cmp_on.log 2>&1
  local ok=1 f nb=0; for f in $P/off.out*; do [ -s "$f" ] && cmp -s "$f" "${f/off.out/on.out}" || ok=0; nb=$((nb + $(stat -c %s "$f" 2>/dev/null || echo 0))); done
  if grep -aq "VERIFY OK" $P/cmp_off.log && grep -aq "VERIFY OK" $P/cmp_on.log && [ -s $P/on.out ] && [ $ok = 1 ]; then echo "cmp4e9 PASS | $nb bytes identical (RNS_STRATEGY=C, off vs on), both VERIFY OK" >> $P/STAGE; else echo "cmp4e9 FAIL | off/on logs: $(grep -ac 'VERIFY OK' $P/cmp_off.log)/$(grep -ac 'VERIFY OK' $P/cmp_on.log) VERIFY OK, cmp ok=$ok" >> $P/STAGE; fi
  rm -f $P/off.out $P/on.out $P/off.out.part* $P/on.out.part*
  fi
  { sed 's/^/on: /' $P/gate_on.txt; sed 's/^/onC: /' $P/gate_onC.txt; grep "t_strategy rc" $P/t_strategy.log | sed 's/^/t_strategy: /'; } >> $P/STAGE
}
stage_ok() { grep -q "^on: gate PASS" $O/stage/STAGE && grep -q "^onC: gate PASS" $O/stage/STAGE && grep -q "t_strategy rc 0" $O/stage/STAGE && grep -q "cmp4e9 PASS" $O/stage/STAGE; }
abba() {   # hold outdir digits stop_epoch run_s half_s -> rc 0 finished, 3 slot ran out (resume), 1 failed
  DRY=$STUB WT=$WT MAXR=$MAXR MINR=3 HALF_S=$6 MARK=K6_DONE STOP_EPOCH=$4 RUN_S=$5 bash $WT/archive/drivers/k6/k6_abba.sh $1 $2 $3 < /dev/null
  local v; v=$(head -1 $2/K6_DONE 2>/dev/null); case "$v" in deadline*) return 3;; SUCCESS*) return 0;; *) return 1;; esac
}
st10=3; st64=3; slot=0; stage_done=0
[ -s $O/stage/STAGE ] && stage_done=1
[ -f $O/abba10/K6_DONE ] && grep -q "^SUCCESS" $O/abba10/K6_DONE && st10=0
[ -f $O/abba64/K6_DONE ] && grep -q "^SUCCESS" $O/abba64/K6_DONE && st64=0
while { [ $stage_done = 0 ] || [ $st10 = 3 ] || [ $st64 = 3 ]; } && [ $(( $(date +%s) + 20 * 60 )) -lt $D ]; do
  slot=$((slot + 1)); mins=$SLOT_MIN; [ $(( $(date +%s) + (mins + 3) * 60 )) -gt $D ] && mins=$(( (D - $(date +%s)) / 60 - 3 ))
  [ $mins -lt 15 ] && { say "no time for a slot"; break; }
  H=$(hold $mins) || { say "slot $slot: no hold (pending too long or sbatch failed)"; sleep 120; continue; }
  end=$(( $(date +%s) + mins * 60 )); say "slot $slot: hold $H, $mins min"
  if [ $stage_done = 0 ]; then stage $H; stage_done=1; say "stage: $(tr '\n' ';' < $O/stage/STAGE)"; stage_ok || { release $H; say "stage FAILED: no ABBA"; break; }; fi
  if [ $st10 = 3 ]; then abba $H $O/abba10 10000000000 $(( end - 90 )) 45 0.3; st10=$?; say "slot $slot abba 1e10 rc $st10: $(head -1 $O/abba10/K6_DONE 2>/dev/null)"; fi
  if [ $st10 != 1 ] && [ $st10 != 3 ] && [ $st64 = 3 ]; then abba $H $O/abba64 64410000000 $(( end - 120 )) 140 1.0; st64=$?; say "slot $slot abba 6.441e10 rc $st64: $(head -1 $O/abba64/K6_DONE 2>/dev/null)"; fi
  release $H; [ $st10 = 1 ] || [ $st64 = 1 ] && break
  sleep 5
done
if ! stage_ok 2>/dev/null; then v="FAILED: stage ($(tr '\n' ';' < $O/stage/STAGE 2>/dev/null))"
elif [ $st10 = 1 ] || [ $st64 = 1 ]; then v="FAILED: 1e10 rc $st10 / 6.441e10 rc $st64 ($(head -1 $O/abba10/K6_DONE 2>/dev/null) ; $(head -1 $O/abba64/K6_DONE 2>/dev/null))"
elif [ $st10 = 0 ] && [ $st64 = 0 ]; then v="SUCCESS: stage PASS; 1e10: $(head -1 $O/abba10/K6_DONE) ; 6.441e10: $(head -1 $O/abba64/K6_DONE)"
else v="PARTIAL: stage PASS; 1e10 rc $st10: $(head -1 $O/abba10/K6_DONE 2>/dev/null) ; 6.441e10 rc $st64: $(head -1 $O/abba64/K6_DONE 2>/dev/null)"; fi
say "$v"; echo "$v" > $O/K6_DONE
