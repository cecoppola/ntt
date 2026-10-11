#!/bin/bash
# s46_arm6.sh (S46) - the workstation-side sleep-until timer for the aac6 soak: nothing touches aac6 before AT (03:31 EDT, after
# the 00:00-03:30 blackout); then it ships branch s46 to aac6 ~/ntt-s46 by bundle, rebuilds there (login node, make -s -j16),
# and starts s46_soak6.sh on the aac6 login node (setsid nohup) with START (03:50 EDT) and D.  Run on this workstation:
#   setsid nohup bash ecalc/s46_arm6.sh > <scratch>/s46_arm6.log 2>&1 < /dev/null &
# env: DRY=1 (print the remote commands, touch nothing), A6PW (file holding the aac6 password), AT (1791703860 = 03:31 EDT), START (1791705000 = 03:50 EDT), D (1791723386 = 08:56 EDT), REPO (this worktree)
AT=${AT:-1791703860}; START=${START:-1791705000}; D=${D:-1791723386}; REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
PW=$(cat "${A6PW:?A6PW: file with the aac6 password (private, never in git)}")
A6="sshpass -p $PW ssh -o ConnectTimeout=20 aac6"; SCP="sshpass -p $PW scp -o ConnectTimeout=20"
[ "${DRY:-0}" = 1 ] && { A6="echo DRY-ssh"; SCP="echo DRY-scp"; }   # DRY=1: print the remote commands only
say() { echo "$(TZ=America/New_York date '+%F %T %Z') $*"; }
say "armed: wait to $(TZ=America/New_York date -d @$AT), soak START $(TZ=America/New_York date -d @$START), D $(TZ=America/New_York date -d @$D)"
while [ $(date +%s) -lt $AT ]; do s=$((AT - $(date +%s))); sleep $((s > 600 ? 600 : s)); done
B=$(mktemp -d)/s46.bundle; git -C "$REPO" bundle create "$B" daeb0f9..s46 || { say "bundle failed"; exit 1; }
$SCP "$B" aac6:~/s46.bundle || { say "scp failed"; exit 1; }
want=$(git -C "$REPO" rev-parse --short=8 s46)
bo=$($A6 "cd ~/ntt-s46 && git fetch -q ~/s46.bundle s46 && git merge -q --ff-only FETCH_HEAD && git log --oneline -1 && bash -lc 'module load rocm; cd ~/ntt-s46/ecalc && make -s -j16' > ~/s46_build2.log 2>&1; echo build rc \$?" < /dev/null); say "$bo"
[ "${DRY:-0}" = 1 ] || echo "$bo" | grep -q 'build rc 0' || { say "aac6 build failed (~/s46_build2.log): not started"; exit 1; }
got=$($A6 "git -C ~/ntt-s46 rev-parse --short=8 HEAD" < /dev/null); [ "${DRY:-0}" = 1 ] && got=$want; [ "$got" = "$want" ] || { say "aac6 clone at $got, want $want: not started"; exit 1; }
$A6 "mkdir -p ~/batch1011/T1/aac6; cd ~/ntt-s46/ecalc && [ -x ecalc ] && START=$START D=$D setsid nohup bash s46_soak6.sh > ~/batch1011/T1/aac6/driver.log 2>&1 < /dev/null & sleep 5; tail -2 ~/batch1011/T1/aac6/summary.txt" < /dev/null
say "soak started on aac6 (marker ~/batch1011/T1/aac6/S46S_DONE)"
