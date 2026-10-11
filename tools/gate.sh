#!/bin/bash
# tools/gate.sh - the standard build-and-digit gate of ecalc on aac7 (Cray SHMEM, 1..4 nodes) and aac6 (single node).
#
# USAGE
#   tools/gate.sh [--build] [--nodes 1|2|4] [--digits 1e9] [--hold JOBID | --alloc] [--line aac7|target|none|file:PATH]
#                 [--env "K=V ..."] [--out DIR] [--ref FILE] [--timeout SEC] [--wait SEC]
#     --build     first `make -B` in the repo's ecalc/ (aac7 with Cray SHMEM: SHMEM_CRAY=1 GMP_HOME=~/gmp, the log must have
#                 -DCOMM_SHMEM for --nodes > 1; elsewhere a plain build, and --nodes > 1 is refused)
#     --hold J    run inside the existing allocation J (srun --overlap through mnrun.sh with SLURM_JOB_ID=J, no -w); never cancelled.
#                 Without --hold/--alloc an exported SLURM_JOB_ID is used as the hold.
#     --alloc     submit its own short sbatch sleep job of --nodes nodes (MNRUN_EXCLUDE_HOSTS -> sbatch -x), run, scancel only that id
#     --line      launch line: aac7 = the LINE of ecalc/e16_headline.sh (parsed, not copied; default on aac7), none = plain (default
#                 elsewhere), target = the target export block, which is NOT in this repo: use file:PATH (K=V per line, # comments),
#                 e.g. --line file:~/target_line.env.   --env words are appended; ECALC_SEGV_TRACE=1 is always set.
#     --ref FILE  reference digits (default ~/ref/e_1000000000.txt, else ecalc/results/e_1000000000.out); only 1e9 has a default
#     --out DIR   output dir (default ~/gate/<timestamp>): build.log, run.log, gate.txt (one line), digit files are deleted after
#     --timeout   seconds allowed for the run (default 1500); --wait seconds to wait for an --alloc job to start (default 1800)
#   Exit 0 = PASS (rc 0, VERIFY OK, digits identical, no segfault), 1 = FAIL.  Kills only its own srun PID and its own sbatch id.
#   Examples:  tools/gate.sh --build --nodes 1 --hold 12463        tools/gate.sh --nodes 2 --alloc --line aac7
#              tools/gate.sh --build --nodes 1 --alloc --line none          (aac6, single node)
# Never put credentials or target values in this file.

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/.." && pwd); E=$ROOT/ecalc
BUILD=0 NODES=1 DIGITS=1e9 HOLD= ALLOC=0 LINE_SEL= EXTRA= OUT= REF= TMO=1500 WAITS=1800
while [ $# -gt 0 ]; do
  case $1 in
    --build) BUILD=1;; --nodes) NODES=$2; shift;; --digits) DIGITS=$2; shift;; --hold) HOLD=$2; shift;; --alloc) ALLOC=1;;
    --line) LINE_SEL=$2; shift;; --env) EXTRA="$EXTRA $2"; shift;; --out) OUT=$2; shift;; --ref) REF=$2; shift;;
    --timeout) TMO=$2; shift;; --wait) WAITS=$2; shift;;
    -h|--help) sed -n '2,/^# Never put/p' "$0" | cut -c3-; exit 0;;
    *) echo "gate.sh: unknown option $1" >&2; exit 2;;
  esac; shift
done
case $NODES in 1|2|4) ;; *) echo "gate.sh: --nodes must be 1, 2 or 4" >&2; exit 2;; esac
[ -z "$HOLD" ] && [ "$ALLOC" = 0 ] && HOLD=${SLURM_JOB_ID:-}
[ -n "$HOLD" ] && [ "$ALLOC" = 1 ] && { echo "gate.sh: --hold and --alloc are exclusive" >&2; exit 2; }
[ -n "$HOLD" ] || [ "$ALLOC" = 1 ] || { echo "gate.sh: give --hold JOBID or --alloc" >&2; exit 2; }
DIG=$(awk -v d="$DIGITS" 'BEGIN { printf "%.0f", d }'); [ "$DIG" -gt 0 ] 2>/dev/null || { echo "gate.sh: bad --digits $DIGITS" >&2; exit 2; }
CRAY=0; [ -d /opt/cray/pe/sma ] && CRAY=1
[ -n "$LINE_SEL" ] || { [ $CRAY = 1 ] && LINE_SEL=aac7 || LINE_SEL=none; }
[ -n "$OUT" ] || OUT=$HOME/gate/$(date +%Y%m%d_%H%M%S); mkdir -p "$OUT" || exit 2; OUT=$(cd "$OUT" && pwd)
if [ -z "$REF" ]; then
  if [ -r "$HOME/ref/e_1000000000.txt" ]; then REF=$HOME/ref/e_1000000000.txt; else REF=$E/results/e_1000000000.out; fi
fi
SUM=$OUT/gate.txt; JOB= ; SPID= ; WPID= ; T0=$(date +%s); RES=FAIL; WHY=; ENVSHOW=
say() { echo "gate.sh: $*"; }
cleanup() {
  [ -n "$WPID" ] && kill "$WPID" 2>/dev/null
  [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null && kill "$SPID" 2>/dev/null     # only our own srun PID
  [ -n "$JOB" ] && scancel "$JOB" 2>/dev/null                                   # only the job this script submitted
  rm -rf "$OUT"/e.out "$OUT"/e.out.part* "$OUT"/e.out.* 2>/dev/null
}
finish() { # RES WHY
  local tot=$(( $(date +%s) - T0 ))
  echo "gate $RES | nodes $NODES | digits $DIG | total ${tot}s${WHY:+ | $WHY} | $(TZ=America/New_York date '+%F %H:%M %Z') | env: ${ENVSHOW:-none}" > "$SUM"
  cat "$SUM"; cleanup; trap - EXIT; [ "$RES" = PASS ] && exit 0 || exit 1; }
fail() { WHY=$*; RES=FAIL; finish; }
trap 'WHY=${WHY:-interrupted}; RES=FAIL; finish' INT TERM
trap '[ -n "$JOB$SPID" ] && cleanup' EXIT

# ---- modules (non-login shells lack `module`)
if ! type module >/dev/null 2>&1; then for f in /usr/share/lmod/lmod/init/bash /etc/profile.d/modules.sh /usr/share/Modules/init/bash /opt/cray/pe/lmod/lmod/init/bash; do [ -r $f ] && { . $f; break; }; done; fi
[ -f "$E/aac7env.sh" ] && [ $CRAY = 1 ] && { cd "$E" && . ./aac7env.sh >/dev/null 2>&1; }

# ---- build
if [ $BUILD = 1 ]; then
  cd "$E" || fail "no $E"
  if [ $CRAY = 1 ]; then
    module load cray-dsmml cray-openshmemx >/dev/null 2>&1
    say "build (Cray SHMEM) in $E, log $OUT/build.log"
    make -B -j8 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp > "$OUT/build.log" 2>&1 || fail "build failed (see build.log)"
    [ "$NODES" = 1 ] || grep -q -e -DCOMM_SHMEM "$OUT/build.log" || fail "build log lacks -DCOMM_SHMEM (multi-node needs SHMEM)"
  else
    [ "$NODES" = 1 ] || fail "no Cray SHMEM here: --nodes > 1 refused"
    command -v hipcc >/dev/null 2>&1 || module load rocm >/dev/null 2>&1
    say "build (plain) in $E, log $OUT/build.log"
    make -B -j16 > "$OUT/build.log" 2>&1 || fail "build failed (see build.log)"
  fi
fi
[ -x "$E/ecalc" ] && [ -x "$ROOT/tools/unpack_digits" ] || fail "ecalc or tools/unpack_digits missing (use --build)"
if [ "$NODES" != 1 ]; then
  [ $CRAY = 1 ] && readelf -d "$E/ecalc" 2>/dev/null | grep -q libsma || fail "ecalc is not linked with SHMEM (multi-node refused)"
fi
[ -r "$REF" ] || fail "reference $REF not readable (--ref)"

# ---- launch line
LINE=
case $LINE_SEL in
  aac7) LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$E/e16_headline.sh") ; [ -n "$LINE" ] || fail "no LINE in e16_headline.sh";;
  none) ;;
  target) fail "--line target: target values are not in the repo; use --line file:PATH";;
  file:*) lf=${LINE_SEL#file:}; lf=${lf/#\~/$HOME}; [ -r "$lf" ] || fail "line file $lf unreadable"
          LINE=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//;s/[[:space:]]*$//' -e 's/^export //' "$lf" | grep -v '^$' | tr '\n' ' ');;
  *) fail "bad --line $LINE_SEL";;
esac
ENVSHOW="line=${LINE_SEL%%:*} $EXTRA ECALC_SEGV_TRACE=1"
ENVSHOW=$(echo $ENVSHOW)

# ---- allocation
if [ "$ALLOC" = 1 ]; then
  PART=${AAC7_PART:-PPAC_MI300A_SPX}; TL=${GATE_ALLOC_MIN:-45}
  xo=; [ -n "${MNRUN_EXCLUDE_HOSTS:-}" ] && xo="-x $MNRUN_EXCLUDE_HOSTS"
  if [ $CRAY = 1 ]; then so="--exclusive -c 192 --gpus-per-node=4"; else so="--gpus=4"; fi
  JOB=$(sbatch -p "$PART" -N "$NODES" $so $xo -t 0:$TL:00 -J gate --parsable --wrap "sleep $((TL * 60))" < /dev/null 2>"$OUT/sbatch.err") || fail "sbatch failed: $(head -c 200 "$OUT/sbatch.err")"
  JOB=${JOB%%;*}; say "own job $JOB ($NODES nodes, $PART, excluding ${MNRUN_EXCLUDE_HOSTS:-nothing}); waiting up to ${WAITS}s"
  w=$WAITS; while [ $w -gt 0 ]; do st=$(squeue -j "$JOB" -h -o %T 2>/dev/null); [ "$st" = RUNNING ] && break; [ -z "$st" ] && fail "job $JOB vanished"; sleep 5; w=$((w - 5)); done
  [ "$st" = RUNNING ] || fail "job $JOB not running after ${WAITS}s"
  J=$JOB
else
  J=$HOLD
  st=$(squeue -j "$J" -h -o %T 2>/dev/null); [ "$st" = RUNNING ] || fail "hold $J is not RUNNING ($st)"
  have=$(squeue -j "$J" -h -o %D); [ "$have" -ge "$NODES" ] 2>/dev/null || fail "hold $J has $have node(s), need $NODES"
fi
say "job $J nodes: $(squeue -j "$J" -h -o %N)"

# ---- run (mnrun.sh execs srun, so $! is our srun PID)
cd "$E" || fail "cd"
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
OF=$OUT/e.out; RL=$OUT/run.log; rm -f "$OF" "$OF".part*
say "run: mnrun.sh $NODES env <line> $EXTRA ECALC_SEGV_TRACE=1 ./ecalc $DIG $OF"
t1=$(date +%s)
SLURM_JOB_ID=$J MNRUN_NODES=$NODES MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) \
  ./mnrun.sh "$NODES" env $LINE $EXTRA ECALC_SEGV_TRACE=1 ./ecalc "$DIG" "$OF" > "$RL" 2>&1 < /dev/null &
SPID=$!
( sleep "$TMO"; kill "$SPID" 2>/dev/null ) & WPID=$!
wait "$SPID"; RC=$?; kill "$WPID" 2>/dev/null; WPID=; SPID=
RUNS=$(( $(date +%s) - t1 ))

# ---- checks
PL=$OUT/run.plain; sed -E 's/^ *[0-9]+: //' "$RL" > "$PL"
V=none; grep -aqE 'VERIFY OK|all [0-9]+ nodes: VERIFY OK' "$PL" && V="VERIFY OK"
SEGV=$(grep -a -c 'Segmentation fault\|ecalc: SEGV \[' "$RL")
TOT=$(grep -a -m1 '^total' "$PL" | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 "s" : "NA"}'); TOT=${TOT:-NA}
CMP=skipped
[ "$RC" = 0 ] && CMP=$("$E/digcmp.sh" "$OF" "$REF" 2>&1 | tail -1)
say "rc $RC | $V | digits $CMP | segv $SEGV | ecalc total $TOT | wall ${RUNS}s"
WHY="rc $RC, $V, digits $CMP, segv $SEGV, ecalc total $TOT, run ${RUNS}s"
if [ "$RC" = 0 ] && [ "$V" = "VERIFY OK" ] && [ "$CMP" = identical ] && [ "$SEGV" = 0 ]; then RES=PASS; fi
finish
