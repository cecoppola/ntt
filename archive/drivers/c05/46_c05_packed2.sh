#!/bin/bash
# OWNER=c05
# NODES=2
# EST_MIN=15
# APPROVED=coordinator-2026-10-11
# c05 (results/C05.md): the 60-bit packed output (ECALC_OUT_PACKED=2, branch c05, commit 5248c31) on 2 real nodes: 1e9 (and 1e10 if time) in formats 2 and 1,
#  digcmp vs ~/ref, RECHECK on the packed parts, one forced-correction run with ECALC_CORR_PATCH=1 at size 2 (formats 0/1/2, 1 and 2 compared with 0).
#  Binary: ~/ntt-wt/c05 (SHMEM build on the login node). Outputs only ~/sess1011c/c05/aac7/. Exits by itself. STUB=1: dry run.
WT=$HOME/ntt-wt/c05; E=$WT/ecalc; O=$HOME/sess1011c/c05/aac7; mkdir -p $O
for f in $E/ecalc $E/aac7env.sh $WT/tools/unpack_digits $HOME/ref/e_1000000000.txt $HOME/ref/e_1e11.out; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would run 2-node ecalc 1e9/1e10 fmt 2,1 + patch into $O"; bash -n $0 && echo syntax ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; cd $E; source aac7env.sh >/dev/null 2>&1; T0=$(date +%s); rm -f $O/summary.txt $O/out.* ; : > $O/summary.txt
S() { echo "$(date +%H:%M:%S) $*" >> $O/summary.txt; }
ENVS="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1"
run() { local tag=$1 d=$2 fmt=$3; shift 3
  SLURM_JOB_ID=$J timeout 600 ./mnrun.sh 2 env $ENVS ECALC_OUT_PACKED=$fmt "$@" ./ecalc $d $O/out.$tag > $O/$tag.log 2>&1 < /dev/null; local rc=$?
  S "run $tag d=$d fmt=$fmt rc=$rc: $(grep -a -c 'VERIFY OK' $O/$tag.log) x VERIFY OK; $(grep -a -h 'wrote ' $O/$tag.log | sed 's/.*(\(.*\))/\1/' | tr '\n' '|' | cut -c1-200); $(grep -a -h '^total' $O/$tag.log | head -1 | cut -c1-40)"; return $rc; }
cmpf() { S "digcmp $1 vs $(basename $2): $($WT/ecalc/digcmp.sh $O/out.$1 $2)  ($(du -cb $O/out.$1* | tail -1 | cut -f1) bytes)"; }
rck() { SLURM_JOB_ID=$J timeout 600 ./mnrun.sh 2 env $ENVS ECALC_OUT_PACKED=$2 ECALC_RECHECK=1 ./ecalc $3 $O/out.$1 > $O/rc_$1.log 2>&1 < /dev/null; S "RECHECK $1 rc=$?: $(grep -a -i 'RECHECK OK\|recheck.*FAIL' $O/rc_$1.log | head -2 | tr '\n' '|' | cut -c1-200)"; }
R9=$HOME/ref/e_1000000000.txt
run a9_f2 1000000000 2; cmpf a9_f2 $R9; rck a9_f2 2 1000000000
run a9_f1 1000000000 1; cmpf a9_f1 $R9
rm -f $O/out.a9_*
# forced correction at size 2 (the KP/EW tests' 108388422 with a +36 correction), the early writer as default
for f in 0 1 2; do run c_f$f 108388422 $f ECALC_CORR_PATCH=1 ECALC_TEST_CORR=36; done
for f in 1 2; do S "digcmp c_f$f vs c_f0: $($WT/ecalc/digcmp.sh $O/out.c_f$f $O/out.c_f0)"; done
rck c_f2 2 108388422
rm -f $O/out.c_*
# 1e10 if there is time (the reference is the prefix of the 1e11 reference; ~10 GB, removed after)
if [ $(( $(date +%s) - T0 )) -lt 540 ] && [ "$(df --output=avail -BG $HOME | tail -1 | tr -dc 0-9)" -gt 60 ]; then
  (head -c 10000000002 $HOME/ref/e_1e11.out; echo) > $O/ref10.txt
  run a10_f2 10000000000 2; cmpf a10_f2 $O/ref10.txt; run a10_f1 10000000000 1; cmpf a10_f1 $O/ref10.txt
  rm -f $O/out.a10_* $O/ref10.txt
else S "1e10 skipped (time/space)"; fi
S "all steps done"; squeue -s -j $J -h 2>/dev/null | grep -v "batch\|extern" | head -3 >> $O/summary.txt
