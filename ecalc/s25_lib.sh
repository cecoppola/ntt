#!/bin/bash
# s25_lib.sh - helpers shared by s25_batch.sh, s26_node.sh and s26_cpx.sh (S25/S26, 2026-10-08).  Source it AFTER setting OUT, WT, E, BUILD_REV and sourcing
# tools/rundriver.sh + rd_init.  Nothing runs on sourcing.  Copied next to the drivers in ~/s25 by the arming commands (not read from the worktree).
alert() { { echo "$(TZ=America/New_York date) $*"; } >> $OUT/ALERT; }
fail() { alert "$*"; RD_VERDICT="FAILED: $*"; exit 0; }
left_s() { local t; t=$(squeue -j $1 -h -o %L -t R 2>/dev/null); [ -z "$t" ] && { echo 0; return; }
  echo "$t" | awk -F'[-:]' '{n=NF; s=$n+60*$(n-1); if(n>=3) s+=3600*$(n-2); if(n>=4) s+=86400*$(n-3); print s}'; }
# check_binary "<strings the ecalc binary must contain>" "<test binaries under ecalc/ that must exist>": rev, freshness, strings, tools
check_binary() {
  [ -e $WT/.git ] || fail "no worktree $WT"
  git -C $WT rev-parse HEAD | grep -q "^$BUILD_REV" || fail "worktree HEAD $(git -C $WT rev-parse --short HEAD) is not BUILD_REV $BUILD_REV"
  git -C $WT diff --quiet -- ecalc || fail "tracked files in $WT/ecalc are modified"
  [ -x $E/ecalc ] || fail "no $E/ecalc"
  [ -z "$(find $E -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name Makefile \) -newer $E/ecalc | head -1)" ] || fail "a source in $E is newer than the ecalc binary (rebuild needed)"
  local s f
  for s in $1; do grep -aqF -- "$s" $E/ecalc || fail "ecalc binary lacks the string '$s'"; done
  for f in $2; do [ -x $E/$f ] || fail "no $E/$f"; done
  [ -x $WT/tools/unpack_digits ] || fail "no $WT/tools/unpack_digits (make tools)"
  [ -x $E/digcmp.sh ] && [ -x $E/mnrun.sh ] || fail "digcmp.sh / mnrun.sh missing in $E"
  rd_say "binary OK: worktree $(git -C $WT rev-parse --short HEAD) = BUILD_REV $BUILD_REV, ecalc $(stat -c %y $E/ecalc | cut -c1-19)"; }
# the line of the aac7 10-node runs: S22's LINE with the user's 2026-10-08 base (DM_MN_LEAN=1, MN_T_CHUNK_MB=2048, DIST_CHUNKS default 8)
LINE0="COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 COMM_OFI_PLAN_CXI=1"
BASE="$LINE0 DM_MN_LEAN=1 MN_T_CHUNK_MB=2048"     # later assignment wins in env(1); DIST_CHUNKS unset (default 8)
