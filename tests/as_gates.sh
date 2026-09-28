#!/bin/bash
# AS (Phase 15): the gates for BS_ARENA_ROOM, unattended, one 1-node job (-J AS, <= 45 min).  On aac6:
#   setsid nohup bash ~/ntt-AS15/tests/as_gates.sh <commit> <outdir> [sbatch options] > <outdir>.out 2>&1 < /dev/null &
# BS_ARENA_ROOM=0.16 ./mnaccept.sh $J --stress --only unit,e9,mn,stress  (unit: t_ntt, t_mul, t_bs, t_dbig 0, t_newton, t_verify,
# t_out, t_mn_grid; e9 in both bases; mn: 10^8 at sizes 2-4, 10^9 at 2 and 4; stress: 10 x 10^9 at size 4, a pool forced to grow)
set -u
COMMIT=$1; OUT=$2; shift 2; SBOPT=("$@"); CLONE=${AS_CLONE:-$HOME/ntt-AS15}; E=$CLONE/ecalc
mkdir -p "$OUT"; log() { echo "$(date '+%F %T %Z') $*"; }
HEAD=$(git -C "$CLONE" rev-parse --short=7 HEAD)
[ "${HEAD:0:7}" = "${COMMIT:0:7}" ] || { log "WRONG COMMIT: $CLONE is at $HEAD, want $COMMIT"; exit 1; }
bash -lc "module load rocm; cd $E && make -s -j16" > "$OUT/build.log" 2>&1 || { log "build failed"; exit 1; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J AS --parsable "${SBOPT[@]}" --wrap "sleep 2700") || exit 1
trap 'scancel $J 2>/dev/null' EXIT
log "job $J submitted"
while :; do st=$(squeue -h -j $J -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { log "job $J vanished"; exit 1; }; sleep 20; done
log "job $J running on $(squeue -h -j $J -o %N)"
cd "$E"
BS_ARENA_ROOM=0.16 ./mnaccept.sh $J --stress --only unit,e9,mn,stress > "$OUT/room016.txt" 2>&1
log "BS_ARENA_ROOM=0.16: rc $? ($(grep -c '^PASS' "$OUT/room016.txt") PASS, $(grep -c '^FAIL' "$OUT/room016.txt") FAIL)"
grep -h '^PASS\|^FAIL' "$OUT"/room016.txt
scancel $J; log "job $J cancelled"; log done
