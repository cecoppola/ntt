#!/bin/bash
# DOC215: login-node plan / layout runs at be2eec3 (~/ntt-int15g, read-only), the launch line's environment
set -u
E=~/ntt-int15g/ecalc/ecalc
O=~/DOC215
mkdir -p $O
cd ~/ntt-int15g/ecalc
LL="COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 ECALC_MEM_GUARD_GB=6 RNS_DIST_CACHE_FIT=1"
hdr() { echo "# $(date '+%Y-%m-%d %H:%M %Z') $(git -C ~/ntt-int15g log --oneline -1) env: $*"; }
run() { local f=$1; shift; { hdr "$@"; env "$@"; echo "rc $?"; } > $O/$f 2>&1; }
# the plan at the target and one step below (auto; np 4 for comparison)
run plan_51e13_auto.txt  ECALC_NP=auto $LL MN_PLAN_ONLY=51000000000000:576 $E
run plan_474e13_auto.txt ECALC_NP=auto $LL MN_PLAN_ONLY=47400000000000:576 $E
run plan_51e13_np4.txt   ECALC_NP=4    $LL MN_PLAN_ONLY=51000000000000:576 $E
run plan_51e13_auto_nofit.txt ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_PLAN_ONLY=51000000000000:576 $E
# the sweep with auto
{ hdr ECALC_NP=auto $LL
  for d in 47000000000000 47400000000000 47500000000000 50500000000000 51000000000000 51100000000000 51200000000000 51700000000000; do
    echo "== $d"; env ECALC_NP=auto $LL MN_PLAN_ONLY=$d:576 $E | grep -E "plan (summary|check|REFUSED|pool|primes|cache)"
  done; } > $O/sweep_auto.txt 2>&1
# the layouts (BS_ARENA_ROOM default 0.16 at be2eec3)
L="4e10:1,1e11:1,88541666666.6667:576,82291666666.6667:576,91694091804:1"
run layout_auto.txt  ECALC_NP=auto $LL BS_LAYOUT_ONLY=$L $E 51000000000000 /dev/null
run layout_np4.txt   ECALC_NP=4    $LL BS_LAYOUT_ONLY=$L $E 51000000000000 /dev/null
run layout_np3.txt   ECALC_NP=3    $LL BS_LAYOUT_ONLY=4e10:1,1e11:1,91694091804:1 $E 51000000000000 /dev/null
run layout_auto_room0.txt ECALC_NP=auto $LL BS_ARENA_ROOM=0 BS_LAYOUT_ONLY=$L $E 51000000000000 /dev/null
echo done
