#!/bin/bash
# S29 phase 1: XEFF M2 for item X3 on 2 nodes of hold 12377 (nodes of soak roles A and C; role B untouched).
#   stops soaks A and C (touch STOP), waits for their ~/s26/<node>_DONE, runs t_comm --bw 4 5 256 sym ABBA x3 with COMM_OFI_DC=1 (base) / 0 (diagnostic, unsafe), then t_comm VERIFY (DC=1, DC=0),
#   then ALWAYS restarts soaks A and C (mv ~/s26/<role> ~/s26/<role>_run1). Output ~/s29m2: summary.txt, bw.txt, log/, M2_DONE.  Binaries: ~/ntt-wt/s29 (built by hand).
J=12377; NA=x9000c1s5b0n0; NC=x9000c1s6b0n0
HD=$(cd "$(dirname "$0")" && pwd); WT=$HOME/ntt-wt/s29; E=$WT/ecalc; OUT=$HOME/s29m2
mkdir -p $OUT/log; rm -f $OUT/M2_DONE
source $WT/tools/rundriver.sh; rd_init $OUT M2_DONE
restart() { rd_say "restarting soaks A and C"
  for r in A C; do [ -d ~/s26/$r ] && mv ~/s26/$r ~/s26/${r}_run1; done
  cd ~/s25 && for r in A C; do setsid nohup bash ~/s25/s26_node.sh $r > ~/s26/$r.nohup2 2>&1 < /dev/null & done; sleep 2; rd_say "soaks restarted"; }
fin() { restart; rd_finish; }
trap fin EXIT
rd_say "stopping soaks A and C"; touch ~/s26/A/STOP ~/s26/C/STOP
for i in $(seq 1 120); do [ -e ~/s26/${NA}_DONE ] && [ -e ~/s26/${NC}_DONE ] && break; sleep 30; done
[ -e ~/s26/${NA}_DONE ] && [ -e ~/s26/${NC}_DONE ] || { RD_VERDICT="FAILED: soaks A/C did not end in 60 min"; exit 0; }
rd_say "soaks ended: $(head -c 120 ~/s26/${NA}_DONE) | $(head -c 120 ~/s26/${NC}_DONE)"
export SLURM_JOB_ID=$J MNRUN_NODELIST=$NA,$NC MNRUN_NODES=2; cd $E; source aac7env.sh >/dev/null 2>&1
rd_health $J $NA $NC || { RD_VERDICT="FAILED: unhealthy nodes"; exit 0; }
ENVL="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 COMM_OFI_PLAN_CXI=1 COMM_OFI_VERBOSE=1 COMM_SHMEM_POOL_MB=8192 COMM_OFI_POOL_MB=8192"
: > $OUT/bw.txt
for rep in 1 2 3; do for dc in 1 0 0 1; do   # ABBA x3 (A = DC 1, B = DC 0)
  lab=bw_r${rep}_dc${dc}_$RANDOM; rd_run $lab 300 ./mnrun.sh 2 env $ENVL COMM_OFI_DC=$dc ./tests/t_comm --bw 4 5 256 sym; rc=$?
  { echo "== rep $rep dc=$dc rc=$rc"; grep -a "t_comm bw: pe 0" $RD_LAST_LOG | cut -c1-230; } >> $OUT/bw.txt
  [ $rc = 0 ] || rd_say "WARNING $lab rc=$rc"; done; done
rd_run verify_dc1 300 ./mnrun.sh 2 env $ENVL COMM_OFI_DC=1 ./tests/t_comm; rd_say "VERIFY dc=1 rc=$RD_LAST_RC: $(grep -a -c 'VERIFY OK' $RD_LAST_LOG) OK lines"
rd_run verify_dc0 300 ./mnrun.sh 2 env $ENVL COMM_OFI_DC=0 ./tests/t_comm; rd_say "VERIFY dc=0 rc=$RD_LAST_RC: $(grep -a -c 'VERIFY OK' $RD_LAST_LOG) OK lines (unsafe mode)"
RD_VERDICT="SUCCESS: M2 bw in $OUT/bw.txt"
