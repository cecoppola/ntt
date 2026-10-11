#!/bin/bash
# runb.sh <label> <K=V ...> : one 4-node 1e9 run in hold, aac7 line, log to ~/sess1011c/a13/B/<label>.log
export SLURM_JOB_ID=$(cat ~/sess1011c/a13/hold); L=$1; shift
cd ~/ntt-wt/a13/ecalc; source aac7env.sh >/dev/null 2>&1
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ e16_headline.sh); [ -n "$NOLINE" ] && LINE="${NOLINE_ENV:-}"
mkdir -p ~/sess1011c/a13/B
MNRUN_NODES=4 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 900)) timeout 600 ./mnrun.sh 4 env $LINE ECALC_SEGV_TRACE=1 "$@" ./ecalc ${DIG:-1000000000} > ~/sess1011c/a13/B/$L.log 2>&1 < /dev/null
echo "rc $?" >> ~/sess1011c/a13/B/$L.log
