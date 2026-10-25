#!/bin/bash
# tools/gate.sh - the standard build-and-digit gate of ecalc on aac7 (xshmem: libfabric or Cray SHMEM, 1..6 nodes) and aac6 (single node).
#
# USAGE
#   tools/gate.sh [--build] [--nodes 1..6] [--digits 1e9] [--hold JOBID | --alloc] [--line aac7|none]
#                 [--profile NAME|PATH] [--env "K=V ..."] [--out DIR] [--ref FILE] [--timeout SEC] [--wait SEC]
#     --build     first `make -B` in the repo's ecalc/ (aac7: path 1 XSHMEM=ofi, the default build, or path 2 XSHMEM=cray; GATE_XSHMEM=cray
#                 selects path 2, default ofi; GMP_HOME=~/gmp; for --nodes > 1 the log must have -DXSHMEM_OFI / -DXSHMEM_CRAY and the
#                 binary the matching library (without --build and GATE_XSHMEM, a binary linked with libsma counts as path 2); elsewhere a plain build, and --nodes > 1 is refused)
#     --hold J    run inside the existing allocation J (srun --overlap through mnrun.sh with SLURM_JOB_ID=J, no -w); never cancelled.
#                 Without --hold/--alloc an exported SLURM_JOB_ID is used as the hold.
#     --alloc     submit its own short sbatch sleep job of --nodes nodes (MNRUN_EXCLUDE_HOSTS -> sbatch -x), run, scancel only that id
#     --line      launch line: aac7 = the launch line of profiles/aac7.conf (LAUNCH_LINE_KEYS, via
#                 tools/profile_load.py aac7 --line; the one source; default on aac7), none = plain (default elsewhere).
#                 --env words are appended; ECALC_SEGV_TRACE=1 is always set.
#     --profile P load profiles/P.conf (tools/profile_load.py) into the gate's environment: explicit env > profile > code default; writes
#                 config_src.txt (config-src: line). Unknown/misspelled key or __FILL__ = FAIL. Not given = behaviour unchanged.
#                 --build also runs `tools/switch_registry.py --check`, `tools/profile_load.py --validate-all`, `tools/xshmem_api_check.py` and
#                 `python3 -m unittest discover -s tests` first (any failing = FAIL; python3 missing = FAIL with a message).
#     (at --nodes > 1 the gate CHECKS the multi-node product form: since D18 (2026-10-12, results/LAUNCH_DEFAULTS.md §DEF) every multi-node product runs the
#                 four-prime 24-digit form P24 (crt.c ec_np_planes; rns_dist.c mn_p24_of), so the run log must show "plane pool 0 for 4 primes";
#                 needs ECALC_NP=auto on the line, or no ECALC_NP. )
#     --ref FILE  reference digits (default ~/ref/e_1000000000.txt, else ecalc/results/e_1000000000.out, else ~/ntt/ecalc/results/...); only 1e9 has a default
#     --out DIR   output dir (default ~/gate/<timestamp>): build.log, run.log, gate.txt (one line), digit files are deleted after
#     --timeout   seconds allowed for the run (default 1500); --wait seconds to wait for an --alloc job to start (default 1800)
#   FIT (D26, results/LAUNCH_DEFAULTS.md §FIT): before the allocation the run's memory fit is printed (tools/plan_values.py --fit, file fit.txt) and a run
#     that does not fit is refused (FAIL, no node time used); GATE_FIT=0 skips it (deliberate edge tests only). mnrun.sh checks again.
#   doccheck: tools/doccheck.py (docs match the tree, <5 s) runs first. GATE_DOCCHECK=strict (default) fails the gate on any problem;
#     GATE_DOCCHECK=warn prints the problems and goes on; GATE_DOCCHECK=off skips it.
#   Exit 0 = PASS (rc 0, VERIFY OK, digits identical, no segfault), 1 = FAIL.  Kills only its own srun PID and its own sbatch id.
#   Examples:  tools/gate.sh --build --nodes 1 --hold 12463        tools/gate.sh --nodes 2 --alloc --line aac7
#              tools/gate.sh --build --nodes 1 --alloc --line none          (aac6, single node)
# Never put credentials or target values in this file.

ARGS=("$@")
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/.." && pwd); E=$ROOT/ecalc
PROFILE= BUILD=0 NODES=1 DIGITS=1e9 HOLD= ALLOC=0 LINE_SEL= EXTRA= OUT= REF= TMO=1500 WAITS=1800
while [ $# -gt 0 ]; do
  case $1 in
    --build) BUILD=1;; --nodes) NODES=$2; shift;; --digits) DIGITS=$2; shift;; --hold) HOLD=$2; shift;; --alloc) ALLOC=1;;
    --line) LINE_SEL=$2; shift;; --profile) PROFILE=$2; shift;; --env) EXTRA="$EXTRA $2"; shift;; --out) OUT=$2; shift;; --ref) REF=$2; shift;;
    --timeout) TMO=$2; shift;; --wait) WAITS=$2; shift;;
    -h|--help) sed -n '2,/^# Never put/p' "$0" | cut -c3-; exit 0;;
    *) echo "gate.sh: unknown option $1" >&2; exit 2;;
  esac; shift
done
case $NODES in [1-6]) ;; *) echo "gate.sh: --nodes must be 1..6 (aac7 QOS)" >&2; exit 2;; esac
[ -z "$HOLD" ] && [ "$ALLOC" = 0 ] && HOLD=${SLURM_JOB_ID:-}
[ -n "$HOLD" ] && [ "$ALLOC" = 1 ] && { echo "gate.sh: --hold and --alloc are exclusive" >&2; exit 2; }
[ -n "$HOLD" ] || [ "$ALLOC" = 1 ] || { echo "gate.sh: give --hold JOBID or --alloc" >&2; exit 2; }
DIG=$(awk -v d="$DIGITS" 'BEGIN { printf "%.0f", d }'); [ "$DIG" -gt 0 ] 2>/dev/null || { echo "gate.sh: bad --digits $DIGITS" >&2; exit 2; }
CRAY=0; [ -d /opt/cray/pe/sma ] && CRAY=1
XS=${GATE_XSHMEM:-ofi}; case $XS in ofi) XSU=OFI; XSLIB=libfabric;; cray) XSU=CRAY; XSLIB=libsma;; *) echo "gate.sh: GATE_XSHMEM must be ofi or cray" >&2; exit 2;; esac
[ -n "$LINE_SEL" ] || { [ $CRAY = 1 ] && LINE_SEL=aac7 || LINE_SEL=none; }
[ -n "$OUT" ] || OUT=$HOME/gate/$(date +%Y%m%d_%H%M%S); mkdir -p "$OUT" || exit 2; OUT=$(cd "$OUT" && pwd)
if [ -z "$REF" ]; then
  for REF in $HOME/ref/e_1000000000.txt $E/results/e_1000000000.out $HOME/ntt/ecalc/results/e_1000000000.out; do [ -r "$REF" ] && break; done
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
if ! type module >/dev/null 2>&1 && [ -z "$GATE_LOGIN" ]; then export GATE_LOGIN=1; exec bash -lc 'exec "$0" "$@"' "$0" "${ARGS[@]}"; fi   # plain ssh: re-run in a login shell (modules)
if ! type module >/dev/null 2>&1; then for f in /usr/share/lmod/lmod/init/bash /etc/profile.d/modules.sh /usr/share/Modules/init/bash /opt/cray/pe/lmod/lmod/init/bash; do [ -r $f ] && { . $f; break; }; done; fi
[ -f "$E/aac7env.sh" ] && [ $CRAY = 1 ] && { cd "$E" && . ./aac7env.sh >/dev/null 2>&1; }

# ---- docs check (cheap, read-only; strict by default)
GATE_DOCCHECK=${GATE_DOCCHECK:-strict}
if [ "$GATE_DOCCHECK" != off ] && command -v python3 >/dev/null 2>&1 && [ -f "$ROOT/tools/doccheck.py" ]; then
  if ! python3 "$ROOT/tools/doccheck.py" > "$OUT/doccheck.log" 2>&1; then
    if [ "$GATE_DOCCHECK" = strict ]; then fail "docs do not match the repository (see doccheck.log; run tools/doccheck.py)"
    else say "WARNING: doccheck found problems ($(tail -1 "$OUT/doccheck.log")); GATE_DOCCHECK=strict would fail"; fi
  fi
fi

# ---- registry and profile validation (before the build; read-only)
if [ $BUILD = 1 ]; then
  command -v python3 >/dev/null 2>&1 || fail "python3 missing (needed for the registry check)"
  python3 "$ROOT/tools/switch_registry.py" --check > "$OUT/registry.log" 2>&1 || fail "switch registry stale (see registry.log; run tools/switch_registry.py)"
  nrw=$(grep -c '^WARN' "$OUT/registry.log" || true); [ "${nrw:-0}" = 0 ] || say "registry: $nrw review-date warning(s), not a failure (registry.log; tools/switch_registry.py --review)"   # T27
  python3 "$ROOT/tools/profile_load.py" --validate-all > "$OUT/profiles.log" 2>&1 || fail "profile validation failed (see profiles.log)"
  python3 "$ROOT/tools/xshmem_api_check.py" > "$OUT/xshmem_api.log" 2>&1 || fail "xshmem API parity check failed (see xshmem_api.log)"
  ( cd "$ROOT" && python3 -m unittest discover -s tests ) > "$OUT/unittest.log" 2>&1 || fail "python unit tests failed (see unittest.log; python3 -m unittest discover -s tests)"
fi
PROF_SRC=
if [ -n "$PROFILE" ]; then
  PL_OUT=$(python3 "$ROOT/tools/profile_load.py" "$PROFILE" 2>"$OUT/profile.err") || fail "profile $PROFILE: $(head -c 200 "$OUT/profile.err")"
  eval "$PL_OUT"
  PROF_SRC="config-src: profile=$ECALC_PROFILE_NAME from-profile=${ECALC_PROFILE_FROM:--} env-over=${ECALC_PROFILE_OVER:--}"
  say "$PROF_SRC"; echo "$PROF_SRC" > "$OUT/config_src.txt"
fi

# ---- build
if [ $BUILD = 1 ]; then
  cd "$E" || fail "no $E"
  if [ $CRAY = 1 ]; then
    module load cray-dsmml cray-openshmemx >/dev/null 2>&1
    say "build (XSHMEM=$XS) in $E, log $OUT/build.log"
    make -B -j8 XSHMEM=$XS GMP_HOME=$HOME/gmp > "$OUT/build.log" 2>&1 || fail "build failed (see build.log)"
    [ "$NODES" = 1 ] || grep -q -e "-DXSHMEM_$XSU" "$OUT/build.log" || fail "build log lacks -DXSHMEM_$XSU (multi-node needs the xshmem build)"
  else
    [ "$NODES" = 1 ] || fail "not a Cray EX system (no /opt/cray/pe/sma): --nodes > 1 refused (the multi-node builds need the Cray PMI and libfabric there)"
    command -v hipcc >/dev/null 2>&1 || module load rocm >/dev/null 2>&1
    say "build (plain) in $E, log $OUT/build.log"
    make -B -j16 > "$OUT/build.log" 2>&1 || fail "build failed (see build.log)"
  fi
fi
[ -x "$E/ecalc" ] && [ -x "$ROOT/tools/unpack_digits" ] || fail "ecalc or tools/unpack_digits missing (use --build)"
if [ "$NODES" != 1 ]; then
  [ $BUILD = 1 ] || [ -n "${GATE_XSHMEM:-}" ] || ! readelf -d "$E/ecalc" 2>/dev/null | grep -q libsma || { XS=cray; XSLIB=libsma; }   # an existing binary: path 2 when it links libsma
  [ $CRAY = 1 ] && readelf -d "$E/ecalc" 2>/dev/null | grep -q "$XSLIB" || fail "ecalc is not linked with $XSLIB (XSHMEM=$XS; multi-node refused)"
fi
[ -r "$REF" ] || fail "reference $REF not readable (--ref)"

# ---- launch line
LINE=
case $LINE_SEL in
  aac7) LINE=$(python3 "$ROOT/tools/profile_load.py" aac7 --line 2>"$OUT/line.err") ; [ -n "$LINE" ] || fail "no launch line in profiles/aac7.conf: $(head -c 200 "$OUT/line.err")";;
  none) ;;
  *) fail "bad --line $LINE_SEL";;
esac
CHECK_NP=0; [ "$NODES" -gt 1 ] && CHECK_NP=1     # D18: the multi-node product form is checked in the log at --nodes > 1
if [ $CHECK_NP = 1 ]; then
  case " $LINE $EXTRA " in *" ECALC_NP=auto "*) ;; *" ECALC_NP="*) fail "--nodes > 1 needs ECALC_NP=auto (or unset) on the line: the P24 form is checked";; esac
fi
ENVSHOW="line=${LINE_SEL%%:*}${PROFILE:+ profile=$PROFILE} $EXTRA ECALC_SEGV_TRACE=1"
ENVSHOW=$(echo $ENVSHOW)

# ---- FIT (user decision D26, results/LAUNCH_DEFAULTS.md §FIT): the memory fit of this run (tools/plan_values.py --fit: the planner's `plan fit` lines),
# before any node time is used; a run that does not fit is refused here (GATE_FIT=0 skips: deliberate edge tests only)
if [ "${GATE_FIT:-1}" != 0 ]; then
  python3 "$ROOT/tools/plan_values.py" --fit --env "$LINE $EXTRA ECALC_SEGV_TRACE=1" --ecalc "$E/ecalc" "$DIG" "$NODES" > "$OUT/fit.txt" 2>&1; frc=$?
  sed 's/^/gate.sh: /' "$OUT/fit.txt"
  [ $frc = 0 ] || fail "memory does not fit (refused before launch; see fit.txt): $(grep -m1 -E 'REFUSED|plan_values:' "$OUT/fit.txt" | cut -c1-200)"
fi

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
say "run: mnrun.sh $NODES env $LINE $EXTRA ECALC_SEGV_TRACE=1 ./ecalc $DIG $OF"
t1=$(date +%s)
SLURM_JOB_ID=$J MNRUN_NODES=$NODES MNRUN_LABEL=1 \
  ./mnrun.sh "$NODES" env $LINE $EXTRA ECALC_SEGV_TRACE=1 ./ecalc "$DIG" "$OF" > "$RL" 2>&1 < /dev/null &
SPID=$!
# watchdog: its sleep runs with stdin/stdout/stderr on /dev/null (a caller piping gate.sh must not wait for it) and is
# killed by its own PID when the watchdog gets TERM after the run ends.
( sleep "$TMO" </dev/null >/dev/null 2>&1 & TPID=$!
  trap 'kill "$TPID" 2>/dev/null; exit 0' TERM
  wait "$TPID" 2>/dev/null && kill "$SPID" 2>/dev/null ) </dev/null >/dev/null 2>&1 & WPID=$!
wait "$SPID"; RC=$?; kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=; SPID=
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
NPOK=
if [ $CHECK_NP = 1 ]; then   # D18: the multi-node form must be visible in the log (pool 0's planes: 4 = P24)
  want='plane pool 0 for 4 primes' tag="P24 in use (pool 0: 4 primes)"
  if grep -aq "$want" "$PL"; then NPOK="; $tag"; else NPOK="; mn form NOT as asked (no '$want' in run.log)"; fi
  WHY="$WHY$NPOK"
fi
if [ "$RC" = 0 ] && [ "$V" = "VERIFY OK" ] && [ "$CMP" = identical ] && [ "$SEGV" = 0 ] && { [ $CHECK_NP = 0 ] || [ -n "${NPOK##*NOT*}" ]; }; then RES=PASS; fi
finish
