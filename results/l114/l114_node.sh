#!/bin/bash
# L1 W1 node batch (Phase 14): one <= 30-min job, unattended.  Start on aac6:  setsid nohup bash ~/l114_node.sh > ~/L114/node.out 2>&1 &
# Waits for its own job (gives up at GIVEUP, CDT), runs the tests under srun, skips a test that would not finish by STOP (CDT), cancels
# the job at exit.  Everything is logged to ~/L114/node_<tag>.log and summarised in ~/L114/node_summary.txt.
set -u
D=~/L114; mkdir -p $D; S=$D/node_summary.txt; E=~/ntt-L114/ecalc; REF9=~/ntt/ecalc/ref/e_1000000000.txt; REF4=~/ntt/ecalc/results/e_4e10.out
GIVEUP=$(date -d "${L114_GIVEUP:-02:58}" +%s); STOP=$(date -d "${L114_STOP:-03:27}" +%s)   # CDT: 03:58 / 04:27 EDT
log() { echo "$(date +%T) $*" | tee -a $S; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:30:00 -J L1 --parsable --wrap "sleep 1800")
log "job $J submitted"
trap 'scancel $J 2>/dev/null; log "job $J cancelled; batch exit"' EXIT
while [ "$(squeue -j $J -h -o %T)" != RUNNING ]; do
  if [ $(date +%s) -ge $GIVEUP ]; then log "GIVE UP: job $J never started by $(date +%T) CDT"; exit 0; fi
  st=$(squeue -j $J -h -o %T); [ -z "$st" ] && { log "job $J vanished"; exit 0; }
  sleep 20
done
T0=$(date +%s); NODE=$(squeue -j $J -h -o %N); log "job $J running on $NODE"
R() { local to=$1 tag=$2; shift 2; timeout $to srun --jobid=$J -N1 --gpus=4 --overlap bash -lc "module load rocm >/dev/null 2>&1; cd $E; $*" > $D/node_$tag.log 2>&1; echo $?; }
N() { srun --jobid=$J -N1 --overlap bash -c "$*"; }
ok() { local est=$1; local now=$(date +%s); [ $((now + est)) -le $STOP ] && [ $((now - T0 + est)) -le 1740 ]; }
cmp9() { N "cmp -s $1 $REF9 && echo identical || echo DIFFERS; rm -f $1 $1.*"; }
# 1. t_newton 20 on the device reciprocal (NEWTON_DEVICE=1: recip_db2), default and tight
for v in "NEWTON_DEVICE=1" "NEWTON_DEVICE=1 DM_TIGHT=1"; do
  ok 240 || { log "SKIP t_newton [$v]"; continue; }
  tag=t_newton_$(echo "$v" | tr ' =' '__')
  rc=$(R 240 $tag "env $v tests/t_newton 20"); n=$(grep -ac 'VERIFY OK' $D/node_$tag.log); f=$(grep -ac 'VERIFY FAILED' $D/node_$tag.log)
  log "t_newton 20 [$v]: rc $rc, VERIFY OK x$n, FAILED x$f; $(grep -a 'RESULT\|total' $D/node_$tag.log | tail -1 | cut -c1-80)"
done
# 2. e9 in both bases with DM_TIGHT=1; and e9 with the device tier forced onto the top levels (BS_MDEV_LOGL=24: the pair release, the top-level flow, the tail retarget)
for spec in "DM_TIGHT=1 LIMB_BASE=10" "DM_TIGHT=1 LIMB_BASE=2" "DM_TIGHT=1 DM_TAIL_DEAD=1 BS_MDEV_LOGL=24 LIMB_BASE=10 RNS_VERBOSE=1 NEWTON_VERBOSE=1"; do
  ok 200 || { log "SKIP e9 [$spec]"; continue; }
  tag=e9_$(echo "$spec" | tr ' =' '__' | cut -c1-40); f=/tmp/l114_$tag.txt
  rc=$(R 200 $tag "env $spec ECALC_VERBOSE=2 ./ecalc 1000000000 $f"); c=$(cmp9 $f); v=$(grep -ac '^VERIFY OK' $D/node_$tag.log)
  log "e9 [$spec]: rc $rc, VERIFY OK x$v, digits $c, $(grep -a '^total' $D/node_$tag.log | tail -1 | cut -c1-40); tail: $(grep -a 'tail moved\|hipMalloc .* inside' $D/node_$tag.log | head -2 | tr '\n' ' ')"
done
# 3. 4e10 with DM_TIGHT=1 against the reference (evicted from the page cache first)
if ok 420; then
  N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; true"
  f=/tmp/l114_4e10.txt; t1=$(date +%s)
  rc=$(R 420 4e10_tight "env DM_TIGHT=1 ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ./ecalc 40000000000 $f"); t2=$(date +%s)
  c=$(N "cmp -s $f $REF4 && echo identical || echo DIFFERS; rm -rf $f $f.*")
  log "4e10 DM_TIGHT=1: rc $rc, VERIFY OK x$(grep -ac '^VERIFY OK' $D/node_4e10_tight.log), digits $c, $(grep -a '^total' $D/node_4e10_tight.log | tail -1 | cut -c1-60), run $((t2 - t1)) s; $(grep -a 'hipMalloc .* inside\|grew by' $D/node_4e10_tight.log | head -2 | tr '\n' ' ')"
  log "   recip: $(grep -a '^recip ' $D/node_4e10_tight.log | cut -c1-70); mem recip: $(grep -a '^mem \[recip\] device' $D/node_4e10_tight.log | cut -c1-200)"
else log "SKIP 4e10 DM_TIGHT=1"; fi
# 4. ECALC_INIT_ONLY at 1e11: the mapped total against BS_LAYOUT_ONLY, tight and the E5 layout
for spec in "DM_TIGHT=1" "DM_TIGHT=1 DM_TAIL_DEAD=2"; do
  ok 150 || { log "SKIP init-only 1e11 [$spec]"; continue; }
  tag=init_$(echo "$spec" | tr ' =' '__'); rc=$(R 150 $tag "env $spec ECALC_INIT_ONLY=1 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ./ecalc 100000000000")
  log "init-only 1e11 [$spec]: rc $rc; $(grep -a '^mem \[init\] device' $D/node_$tag.log | cut -c1-120); $(grep -a 'bs: dm layout' $D/node_$tag.log | cut -c1-220); $(grep -a '^bs: arenas' $D/node_$tag.log | cut -c1-120)"
done
# 5. if time remains: 4e10 with DM_TIGHT=1 DM_TAIL_DEAD=1 (the tail retarget at a real size)
if ok 420; then
  N "python3 -c \"import os,sys; fd=os.open(sys.argv[1],os.O_RDONLY); os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)\" $REF4 2>/dev/null; true"
  f=/tmp/l114_4e10b.txt; t1=$(date +%s)
  rc=$(R 420 4e10_tight_dead "env DM_TIGHT=1 DM_TAIL_DEAD=1 ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ./ecalc 40000000000 $f"); t2=$(date +%s)
  c=$(N "cmp -s $f $REF4 && echo identical || echo DIFFERS; rm -rf $f $f.*")
  log "4e10 DM_TIGHT=1 DM_TAIL_DEAD=1: rc $rc, VERIFY OK x$(grep -ac '^VERIFY OK' $D/node_4e10_tight_dead.log), digits $c, $(grep -a '^total' $D/node_4e10_tight_dead.log | tail -1 | cut -c1-60), run $((t2 - t1)) s; $(grep -a 'tail moved\|hipMalloc .* inside\|grew by' $D/node_4e10_tight_dead.log | head -5 | tr '\n' ' ')"
else log "SKIP 4e10 DM_TIGHT=1 DM_TAIL_DEAD=1"; fi
N "rm -f /tmp/l114_*" 2>/dev/null
log "batch done, $(( $(date +%s) - T0 )) s of the job"
