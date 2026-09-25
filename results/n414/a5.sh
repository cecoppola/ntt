#!/bin/bash
# N414 A5: the dist tier's operand load per call -- node or memory state.  Unattended; cancels its job.
#   setsid nohup bash ~/N414/a5.sh <node> <batch> <steps...> > ~/N414/a5_<batch>.out 2>&1 &
# steps: props:<tag> probe:<tag> short:<tag> fill:<GB> unfill thp:<GB> precompact
NODE=$1; B=$2; shift 2
OUT=~/N414/a5/$B; mkdir -p $OUT; S=~/N414/a5/summary.txt; E=~/ntt-N414/ecalc
log() { echo "[$(date +%H:%M:%S)] [$B $NODE] $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 -w ppac-pl1-$NODE --gpus=4 -t 0:45:00 -J N4 --parsable --wrap "sleep 2700")
log "job $J submitted"
trap "scancel $J; log 'job $J cancelled'" EXIT
until [ "$(squeue -j $J -h -o %T 2>/dev/null)" = RUNNING ]; do sleep 10; [ -z "$(squeue -j $J -h -o %T 2>/dev/null)" ] && { log "job $J gone"; exit 1; }; done
T0=$(date +%s); log "job $J running on $(squeue -j $J -h -o %N)"
left() { echo $(( T0 + 2580 - $(date +%s) )); }
R() { srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; $*"; }
buddy() { R "grep -E 'Normal' /proc/buddyinfo | awk '{s9=0; s10=0; for(i=5;i<=NF;i++){o=i-5; if(o>=9) s9+=\$i*2^(o-9)} printf \"%s node%s: free 2MiB-blocks(order>=9) %.0f GB; \", \$1, \$2, s9*2/1024} END{print \"\"}'; grep -E 'MemFree|^Cached|AnonHugePages|Dirty' /proc/meminfo | tr -s ' ' | paste -sd' '"; }
for st in "$@"; do
  k=${st%%:*}; a=${st#*:}
  case $k in
  props) R "bash ~/N414/props.sh $a" > $OUT/props_$a.txt 2>&1; log "props $a: $(grep -E '^MemFree|^Cached' $OUT/props_$a.txt | tr -s ' ' | paste -sd' ')";;
  probe)
    [ $(left) -lt 330 ] && { log "SKIP probe $a"; continue; }
    IFS=: read -r tg dg ev <<< "$a"; dg=${dg:-142000000000}; ev=${ev//,/ }
    R "for i in \$(seq 1 30); do [ \$(awk '/MemFree/{print \$2}' /proc/meminfo) -gt 500000000 ] && break; sleep 5; done"
    log "before probe $tg ($dg; $ev): $(buddy)"
    R "cd $E; (sleep ${MID_S:-100}; cat /proc/buddyinfo; grep -E 'MemFree|AnonHuge' /proc/meminfo) > $OUT/mid_$tg.txt 2>&1 & env $ev ECALC_PLANE_CAP=2^30 RNS_STRATEGY=C RNS_VERBOSE=1 ECALC_VERBOSE=1 timeout -s TERM ${PROBE_S:-240} /usr/bin/time -v ./ecalc $dg; wait" > $OUT/probe_$tg.log 2>&1
    L=$OUT/probe_$tg.log
    pa=$(grep -a -o 'peer access [0-9.]* s' $L | head -n 1); ma=$(grep -a -o 'malloc [0-9.]* s' $L | head -n 1)
    st=$(grep -a '^dist ' $L | awk '{for(i=1;i<=NF;i++){if($i=="load")l+=$(i+1); if($i=="ntt")t+=$(i+1)} n++} END {if(n) printf "%d dist calls: load %.3f ntt %.3f s per call", n, l/n, t/n; else print "no dist calls"}')
    s30=$(grep -a '^dist 2^30 ' $L | awk '{for(i=1;i<=NF;i++){if($i=="load")l+=$(i+1); if($i=="ntt")t+=$(i+1)} n++} END {if(n) printf "%d at 2^30: load %.3f ntt %.3f", n, l/n, t/n}')
    mid=$(awk '/Normal/{s=0; for(i=5;i<=NF;i++){o=i-5; if(o>=9) s+=$i*2^(o-9)} printf "n%s %.0f ", $2, s*2/1024}' $OUT/mid_$tg.txt 2>/dev/null)
    log "probe $tg ($dg; $ev): $pa, hipMalloc $ma, $st; $s30; $(grep -a -o 'device [0-9.]* GB in use' $L | head -n 1); mid-run free 2MiB-blocks GB: $mid $(grep MemFree $OUT/mid_$tg.txt | tr -s ' ')";;
  short)
    [ $(left) -lt 300 ] && { log "SKIP short $a"; continue; }
    R "cd $E; ECALC_PLANE_CAP=2^30 RNS_STRATEGY=C RNS_VERBOSE=1 timeout -s TERM 400 /usr/bin/time -v ./ecalc 20000000000" > $OUT/short_$a.log 2>&1
    L=$OUT/short_$a.log
    st=$(grep -a '^dist ' $L | awk '{for(i=1;i<=NF;i++){if($i=="load")l+=$(i+1); if($i=="ntt")t+=$(i+1)} n++} END {if(n) printf "%d dist calls: load %.3f ntt %.3f s per call", n, l/n, t/n; else print "no dist calls"}')
    log "short $a (2e10): $(grep -a -o 'peer access [0-9.]* s' $L | head -n 1), $st; $(grep -a '^total' $L | cut -c1-90); $(grep -a -c '^VERIFY OK' $L) VERIFY OK";;
  fill)
    t1=$(date +%s)
    R "d=\$(df -l --output=avail,target -B1G 2>/dev/null | grep -v -E 'Avail|/boot|/dev|/run|/sys' | sort -n | tail -n 1 | awk '{print \$2}'); echo dir \$d; df -h \$d; dd if=/dev/zero of=\$d/n4_fill.$USER bs=64M count=\$(( $a * 16 )) 2>&1 | tail -n 1; echo \$d/n4_fill.$USER > /tmp/n4_fill_path.$USER" > $OUT/fill.txt 2>&1
    log "fill ${a} GB: $(tail -n 1 $OUT/fill.txt), $(( $(date +%s) - t1 )) s; $(buddy)";;
  unfill) R "f=\$(cat /tmp/n4_fill_path.$USER); python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" \$f; rm -f \$f /tmp/n4_fill_path.$USER"; log "unfill: $(buddy)";;
  thp) R "~/N414/thptouch $a" > $OUT/thp_$a.txt 2>&1; log "thp $a: $(cat $OUT/thp_$a.txt | paste -sd' '); $(buddy)";;
  esac
done
log "batch done, $(( $(date +%s) - T0 )) s"
