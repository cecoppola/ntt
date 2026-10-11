#!/bin/bash
# OWNER=c06
# NODES=2
# EST_MIN=27
# APPROVED=coordinator-2026-10-11
# c06 gates (results/C06.md): M3 COMM_LAYER_VSLOT_RELEASE and G5 COMM_LAYER_TEST_SKEW_MB, 2 nodes, 1e9 digits, each gate = VERIFY OK + digits identical to ~/ref.
#  All on the aac7 line (X2 + SHARE + T2048 ...) with DIST_GEN=1 (at 2 nodes every group is a power of two: DIST_GEN forces the general map, so the layered communicator and its v-slots really run).
#  g1 RELEASE=1 | g2 RELEASE=1 + SKEW (the slots' pool offsets differ between the 2 nodes) | g0 control (no RELEASE) | g4 SKEW alone (the existing code, asymmetric offsets) | g3 RELEASE=1 without SHARE.
# Uses the worktree build ~/ntt-wt/c06 (built on the login node).  Outputs only ~/sess1011c/c06/G/.  Exits by itself.  STUB=1: dry run (no srun).
WT=$HOME/ntt-wt/c06; E=$WT/ecalc; O=$HOME/sess1011c/c06/G; mkdir -p $O; : > $O/summary.txt
for f in $E/ecalc $WT/tools/gate.sh $E/mnrun.sh $E/e16_headline.sh; do [ -e $f ] || { echo "missing $f"; exit 2; }; done
strings $E/ecalc | grep -q COMM_LAYER_VSLOT_RELEASE || { echo "ecalc built without M3"; exit 2; }
strings $E/ecalc | grep -q COMM_LAYER_TEST_SKEW_MB || { echo "ecalc built without the skew test switch"; exit 2; }
[ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer $E/ecalc | head -1)" ] || { echo "a source is newer than the ecalc binary"; exit 2; }
if [ "${STUB:-0}" = 1 ]; then echo "STUB ok: would run gate.sh x5 (2 nodes, 1e9) on \$SLURM_JOB_ID; outputs $O"; bash -n $0 && echo syntax ok; exit 0; fi
: ${SLURM_JOB_ID:?need the runner allocation}
J=$SLURM_JOB_ID; T0=$(date +%s)
cd $E; source aac7env.sh >/dev/null 2>&1
gt() { # name env...
  local nm=$1; shift
  [ $(( $(date +%s) - T0 )) -lt 1260 ] || { echo "$nm skipped (time)" | tee -a $O/summary.txt; return; }
  bash $WT/tools/gate.sh --nodes 2 --hold $J --out $O/$nm --timeout 420 --env "$*" < /dev/null > $O/$nm.out 2>&1
  echo "$nm: $(cat $O/$nm/gate.txt 2>/dev/null | cut -c1-300)" | tee -a $O/summary.txt
  grep -a -E "comm_ofi: device .*pool peak|pool peak" $O/$nm/run.plain 2>/dev/null | head -2 | cut -c1-200 >> $O/summary.txt; }
gt g1 DIST_GEN=1 COMM_LAYER_VSLOT_RELEASE=1 COMM_OFI_VERBOSE=1
gt g2 DIST_GEN=1 COMM_LAYER_VSLOT_RELEASE=1 COMM_LAYER_TEST_SKEW_MB=16 COMM_OFI_VERBOSE=1
gt g0 DIST_GEN=1 COMM_OFI_VERBOSE=1
gt g4 DIST_GEN=1 COMM_LAYER_TEST_SKEW_MB=16
gt g3 DIST_GEN=1 COMM_LAYER_VSLOT_SHARE=0 COMM_LAYER_VSLOT_RELEASE=1
echo "DONE $(date)" >> $O/summary.txt
