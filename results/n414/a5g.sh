#!/bin/bash
# N414 A5 gates and full runs for RNS_PLANES_FIRST.  Unattended; cancels its job.
#   setsid nohup bash ~/N414/a5g.sh <node> <batch> <steps...> > ~/N414/a5_<batch>.out 2>&1 &
# steps: e9 (mnaccept --only e9 with the switch) | e4e10 (4e10 with the switch, sha1 vs ~/e_4e10.sha1) | units (t_newton 20, t_mul 20)
#        full:<tag>:<digits>:<envs,...> (a whole run, no outfile)
NODE=$1; B=$2; shift 2
OUT=~/N414/a5/$B; mkdir -p $OUT; S=~/N414/a5/summary.txt; E=~/ntt-N414/ecalc; cd $E
log() { echo "[$(date +%H:%M:%S)] [$B $NODE] $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 -w ppac-pl1-$NODE --gpus=4 -t 0:45:00 -J N4 --parsable --wrap "sleep 2700")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
until [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ]; do sleep 10; [ -z "$(squeue -j $J -h -o %T 2>/dev/null)" ] && { log "job $J gone"; exit 1; }; done
T0=$(date +%s); log "job $J running on $(squeue -j $J -h -o %N)"
left() { echo $(( T0 + 2600 - $(date +%s) )); }
R() { srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*"; }
memwait() { R "for i in \$(seq 1 40); do [ \$(awk '/MemFree/{print \$2}' /proc/meminfo) -gt 500000000 ] && break; sleep 5; done; grep MemFree /proc/meminfo | tr -s ' '"; }
for st in "$@"; do
  k=${st%%:*}; a=${st#*:}
  case $k in
  e9) memwait >/dev/null; RNS_PLANES_FIRST=1 ./mnaccept.sh $J --only e9 > $OUT/e9.txt 2>&1; log "e9 with RNS_PLANES_FIRST=1: $(grep -a '^PASS\|^FAIL' $OUT/e9.txt | paste -sd' ')";;
  units) R "timeout 900 ./tests/t_newton 20" > $OUT/t_newton.log 2>&1; r1=$?; R "timeout 900 ./tests/t_mul 20" > $OUT/t_mul.log 2>&1; r2=$?
         log "t_newton 20: rc $r1, $(grep -ac 'VERIFY OK' $OUT/t_newton.log) OK, $(grep -ac 'VERIFY FAILED' $OUT/t_newton.log) FAILED; t_mul 20: rc $r2, $(grep -ac 'VERIFY OK' $OUT/t_mul.log) OK, $(grep -ac 'VERIFY FAILED' $OUT/t_mul.log) FAILED";;
  e4e10) memwait >/dev/null
    R "rm -f /tmp/n4_4e10.out*; RNS_PLANES_FIRST=1 timeout 900 ./ecalc 40000000000 /tmp/n4_4e10.out" > $OUT/e4e10.log 2>&1; rc=$?
    h=$(R "sha1sum /tmp/n4_4e10.out | cut -c1-40; rm -f /tmp/n4_4e10.out*"); r=$(cut -c1-40 ~/e_4e10.sha1)
    log "4e10 RNS_PLANES_FIRST=1: rc $rc, $([ "$h" = "$r" ] && echo 'IDENTICAL (sha1)' || echo "DIFFERS ($h vs $r)"), $(grep -a -c '^VERIFY OK' $OUT/e4e10.log) VERIFY OK; $(grep -a '^total' $OUT/e4e10.log | cut -c1-100)";;
  full)
    IFS=: read -r tg dg ev <<< "$a"; ev=${ev//,/ }; est=${EST:-900}
    [ $(left) -lt $est ] && { log "SKIP full $tg (left $(left) s)"; continue; }
    memwait >/dev/null
    R "env $ev RNS_VERBOSE=1 timeout -s TERM $(( $(left) - 30 )) /usr/bin/time -v ./ecalc $dg" > $OUT/full_$tg.log 2>&1; rc=$?
    L=$OUT/full_$tg.log
    s30=$(grep -a '^dist 2^30 ' $L | awk '{for(i=1;i<=NF;i++){if($i=="load")l+=$(i+1); if($i=="ntt")t+=$(i+1)} n++} END {if(n) printf "%d at 2^30: load %.3f ntt %.3f", n, l/n, t/n}')
    s31=$(grep -a '^dist 2^31 ' $L | awk '{for(i=1;i<=NF;i++){if($i=="load")l+=$(i+1); if($i=="ntt")t+=$(i+1)} n++} END {if(n) printf "%d at 2^31: load %.3f ntt %.3f", n, l/n, t/n}')
    log "full $tg ($dg; $ev): rc $rc, $(grep -a -c '^VERIFY OK' $L) VERIFY OK; $(grep -a '^total' $L | cut -c1-120); $(grep -a -o 'init: rns_init [0-9.]* s' $L); $s30; $s31; $(grep -a '^recip(db)' $L | cut -c1-110); wall $(grep -a 'Elapsed (wall' $L | awk '{print $NF}')";;
  esac
done
log "batch done, $(( $(date +%s) - T0 )) s"
