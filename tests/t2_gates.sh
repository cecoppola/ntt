#!/bin/bash
# T2 (Phase 15): the gates for BS_SEED_FILL, unattended, one 1-node job (-J T2, <= 45 min).  On aac6:
#   setsid nohup bash t2_gates.sh <commit> <outdir> > <outdir>.out 2>&1 < /dev/null &
# 1. mnaccept.sh --only unit,e9 on the defaults (t_ntt, t_mul, t_bs, t_dbig, t_newton, t_verify, t_out, t_mn_grid; e9 both bases)
# 2. BS_SEED_FILL=128 mnaccept.sh --only e9,mn (e9 both bases; 10^8 at sizes 2-4, 10^9 at 2 and 4: the per-node span)
set -u
COMMIT=$1; OUT=$2; CLONE=${T2_CLONE:-$HOME/ntt-T215}; E=$CLONE/ecalc
mkdir -p "$OUT"; log() { echo "$(date '+%F %T %Z') $*"; }
HEAD=$(git -C "$CLONE" rev-parse --short=7 HEAD)
[ "${HEAD:0:7}" = "${COMMIT:0:7}" ] || { log "WRONG COMMIT: $CLONE is at $HEAD, want $COMMIT"; exit 1; }
bash -lc "module load rocm; cd $E && make -s -j16" > "$OUT/build.log" 2>&1 || { log "build failed"; exit 1; }
J=$(sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J T2 --parsable --wrap "sleep 2700") || exit 1
trap 'scancel $J 2>/dev/null' EXIT
log "job $J submitted"
while :; do st=$(squeue -h -j $J -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && { log "job $J vanished"; exit 1; }; sleep 20; done
log "job $J running on $(squeue -h -j $J -o %N)"
cd "$E"
./mnaccept.sh $J --only unit,e9 > "$OUT/defaults.txt" 2>&1; log "defaults: rc $? ($(grep -c '^PASS' "$OUT/defaults.txt") PASS, $(grep -c '^FAIL' "$OUT/defaults.txt") FAIL)"
BS_SEED_FILL=128 ./mnaccept.sh $J --only e9,mn > "$OUT/fill128.txt" 2>&1; log "BS_SEED_FILL=128: rc $? ($(grep -c '^PASS' "$OUT/fill128.txt") PASS, $(grep -c '^FAIL' "$OUT/fill128.txt") FAIL)"
grep -h '^PASS\|^FAIL' "$OUT"/defaults.txt "$OUT"/fill128.txt
scancel $J; log "job $J cancelled"; log done
