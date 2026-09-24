#!/bin/bash
# L1 W1 node batch 2 (the packing fix 47ced5f): waits for batch 1's job (name L1) to end, rebuilds, one <= 30-min job:
# 4e10 DM_TIGHT=1, 4e10 DM_TIGHT=1 DM_TAIL_DEAD=1 (both cmp'd), e9 x3, then 1e11 DM_TIGHT=1 (VERIFY only) if time remains.
# Start on aac6:  setsid nohup bash ~/l114_node2.sh > ~/L114/node2.out 2>&1 &
set -u
D=~/L114; mkdir -p $D; S=$D/node2_summary.txt; E=~/ntt-L114/ecalc; REF9=~/ntt/ecalc/ref/e_1000000000.txt; REF4=~/ntt/ecalc/results/e_4e10.out
GIVEUP=$(date -d "${L114_GIVEUP:-02:58}" +%s); STOP=$(date -d "${L114_STOP:-03:27}" +%s)   # CDT: 03:58 / 04:27 EDT
log() { echo "$(date +%T) $*" | tee -a $S; }
while [ -n "$(squeue -u chcoppola -n L1 -h -o %i)" ]; do sleep 20; [ $(date +%s) -ge $GIVEUP ] && { log "GIVE UP waiting for batch 1"; exit 0; }; done
bash -lc "module load rocm; cd $E && make -s -j16" > $D/build2.log 2>&1; log "build rc $? ($(cd $E && git log --oneline -1 | cut -c1-60))"
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:30:00 -J L1 --parsable --wrap "sleep 1800")
log "job $J submitted"
trap 'scancel $J 2>/dev/null; log "job $J cancelled; batch exit"' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  if [ $(date +%s) -ge $GIVEUP ]; then log "GIVE UP: job $J never started by $(date +%T) CDT"; exit 0; fi
  st=$(squeue -j $J -h -o %T); [ -z "$st" ] && { log "job $J vanished"; exit 0; }
  sleep 20
done
T0=$(date +%s); NODE=$(squeue -j $J -h -o %N); log "job $J running on $NODE"
R() { local to=$1 tag=$2; shift 2; timeout $to srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*" > $D/node2_$tag.log 2>&1; echo $?; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
ok() { local est=$1; local now=$(date +%s); [ $((now + est)) -le $STOP ] && [ $((now - T0 + est)) -le 1740 ]; }
evict() { N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; true"; }
run4() { local tag=$1 spec=$2; ok 420 || { log "SKIP 4e10 [$spec]"; return; }
  evict; local f=/tmp/l114_$tag.txt t1=$(date +%s)
  rc=$(R 420 $tag "env $spec ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ./ecalc 40000000000 $f"); t2=$(date +%s)
  c=$(N "cmp -s $f $REF4 && echo identical || echo DIFFERS; rm -rf $f $f.*")
  log "4e10 [$spec]: rc $rc, VERIFY OK x$(grep -ac '^VERIFY OK' $D/node2_$tag.log), digits $c, $(grep -a '^total' $D/node2_$tag.log | tail -1 | cut -c1-60), run $((t2 - t1)) s; hipMalloc: $(grep -a 'hipMalloc .* inside\|grew by' $D/node2_$tag.log | head -2 | tr '\n' ' ')"
  log "   $(grep -a '^recip ' $D/node2_$tag.log | cut -c1-60); $(grep -a '^mem \[recip\] device' $D/node2_$tag.log | cut -c1-230)"
  log "   tail: $(grep -a 'tail moved\|APU0 tail' $D/node2_$tag.log | head -2 | tr '\n' ' ')"
  log "   last doublings: $(grep -a 'newton(db) j' $D/node2_$tag.log | tail -2 | cut -c1-160 | tr '\n' ' ')"
}
run4 4e10_tight "DM_TIGHT=1"
run4 4e10_tight_dead "DM_TIGHT=1 DM_TAIL_DEAD=1"
for spec in "DM_TIGHT=1 LIMB_BASE=10" "DM_TIGHT=1 LIMB_BASE=2" "DM_TIGHT=1 DM_TAIL_DEAD=1 BS_MDEV_LOGL=24 LIMB_BASE=10"; do
  ok 200 || { log "SKIP e9 [$spec]"; continue; }
  tag=e9_$(echo "$spec" | tr ' =' '__' | cut -c1-40); f=/tmp/l114_$tag.txt
  rc=$(R 200 $tag "env $spec ./ecalc 1000000000 $f"); c=$(N "cmp -s $f $REF9 && echo identical || echo DIFFERS; rm -f $f $f.*")
  log "e9 [$spec]: rc $rc, VERIFY OK x$(grep -ac '^VERIFY OK' $D/node2_$tag.log), digits $c, $(grep -a '^total' $D/node2_$tag.log | tail -1 | cut -c1-30)"
done
if ok 420; then
  t1=$(date +%s); rc=$(R 420 1e11_tight "env DM_TIGHT=1 ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ./ecalc 100000000000"); t2=$(date +%s)
  log "1e11 DM_TIGHT=1 (no outfile): rc $rc, VERIFY OK x$(grep -ac '^VERIFY OK' $D/node2_1e11_tight.log), $(grep -a '^total' $D/node2_1e11_tight.log | tail -1 | cut -c1-60), run $((t2 - t1)) s; hipMalloc: $(grep -a 'hipMalloc .* inside\|grew by' $D/node2_1e11_tight.log | head -2 | tr '\n' ' ')"
  log "   $(grep -a '^mem \[recip\] device' $D/node2_1e11_tight.log | cut -c1-230)"
  log "   $(grep -a 'APU0 tail' $D/node2_1e11_tight.log | head -1)"
else log "SKIP 1e11"; fi
N "rm -f /tmp/l114_*" 2>/dev/null
log "batch 2 done, $(( $(date +%s) - T0 )) s of the job"
