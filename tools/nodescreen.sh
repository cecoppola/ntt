#!/bin/bash
# tools/nodescreen.sh - FAST screen of every node of an allocation for very badly performing nodes, before a long run (B04, results/B04.md).
# Two stages, each in parallel on ALL nodes, one process per node, never two network programs per node:
#   local  (no network; srun, one task per node): t_nodechk (meminfo + 1 GB host->device upload per APU, median of 3, + the T10 cold path
#          of --cold-gb per APU: VMM map, first touch, one cold upload) then ONE single-node ecalc of --local-digits digits with
#          ECALC_INIT_TL=1 ECALC_VERBOSE=2 (init time = rns_init, total = the short real compute).  Per-node wall of each step is recorded.
#   mn     (one SHMEM job over the nodes the local stage kept; skipped with --no-mn): ecalc of N x --mn-digits-per-node digits with
#          ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 MNRUN_LABEL=1 through mnrun.sh -> tools/initchk.py's per-node rns_init / first-upload (host-attributed)
#          plus the run's wall (a fabric-wide slow state shows as a high total, it cannot be pinned on one node).
# Rule (same as initchk): a node is flagged when a time metric is > THR x the median (default 3) AND >= MINEX s above it (default 2), or when any
# step failed / timed out / printed no result.  With fewer than 3 nodes the reference is the smaller value instead of the median.
# SLURM_LABELIO=1 labels the mn log (mnrun.sh drops MNRUN_LABEL when MNRUN_CPUS_PER_TASK is set: its copt is overwritten, line 72-73).
# Output ($OUT): screen_table.txt (per-node table), EXCLUDE (one host per line, the nodes to leave out), screen_summary.txt (wall per stage),
# local/<host>.log, mn.log.  Exit 0 = no node flagged, 3 = flagged (EXCLUDE is non-empty), 2 = the screen itself could not run / parse.
#
#   tools/nodescreen.sh --hold JOBID [--out DIR] [--cold-gb 4] [--local-digits 1e8] [--mn-digits-per-node 1e8] [--no-mn] [--no-local]
#                       [--timeout-local 120] [--timeout-mn 240] [--thr 3] [--min-excess 2] [--min-avail-gb 0] [--exclude-hosts LIST]
#                       [--flagged-file FILE] [--ecalc-dir DIR]
#   --flagged-file FILE  append the EXCLUDE hosts to FILE (target_kit.sh's $OUT/flagged_hosts.txt: later kit stages then drop them)
#   --exclude-hosts LIST hosts of the allocation never used by the screen (srun -x), reported as such
#   --min-avail-gb G     also flag MemAvailable < G GB (default 0 = off: a low MemAvailable alone was not a stall, results/S49.md)
# Runs inside an existing hold (srun --overlap, no -w, --mem=0 -c1 as the session rules say); kills only its own srun PIDs; never cancels a job.
# No credentials or target values in this file.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/.." && pwd); E=${NS_ECALC_DIR:-$ROOT/ecalc}
HOLD=${SLURM_JOB_ID:-} OUT= COLD=4 LDIG=1e8 MDIG=1e8 DO_MN=1 DO_LOCAL=1 TMO_L=120 TMO_M=240 THR=3 MINEX=2 MINAV=0 EXCL= FLAGF=
while [ $# -gt 0 ]; do
  case $1 in
    --hold) HOLD=$2; shift;; --out) OUT=$2; shift;; --cold-gb) COLD=$2; shift;; --local-digits) LDIG=$2; shift;;
    --mn-digits-per-node) MDIG=$2; shift;; --no-mn) DO_MN=0;; --no-local) DO_LOCAL=0;;
    --timeout-local) TMO_L=$2; shift;; --timeout-mn) TMO_M=$2; shift;; --thr) THR=$2; shift;; --min-excess) MINEX=$2; shift;;
    --min-avail-gb) MINAV=$2; shift;; --exclude-hosts) EXCL=$2; shift;; --flagged-file) FLAGF=$2; shift;; --ecalc-dir) E=$2; shift;;
    -h|--help) sed -n '2,/^# No credentials/p' "$0" | cut -c3-; exit 0;;
    *) echo "nodescreen.sh: unknown option $1" >&2; exit 2;;
  esac; shift
done
[ -n "$HOLD" ] || { echo "nodescreen.sh: give --hold JOBID (or export SLURM_JOB_ID)" >&2; exit 2; }
[ -x "$E/ecalc" ] && [ -x "$E/tests/t_nodechk" ] || { echo "nodescreen.sh: need $E/ecalc and $E/tests/t_nodechk built (make; make tests/t_nodechk)" >&2; exit 2; }
[ -n "$OUT" ] || OUT=$HOME/nodescreen/$(date +%Y%m%d_%H%M%S); mkdir -p "$OUT/local" || exit 2; OUT=$(cd "$OUT" && pwd)
# --local-digits 0 skips the single-node ecalc
DIGL=$(awk -v d="$LDIG" 'BEGIN { printf "%.0f", d }'); DIGM=$(awk -v d="$MDIG" 'BEGIN { printf "%.0f", d }')
T0=$(date +%s.%N); SUM=$OUT/screen_summary.txt; : > "$SUM"; SPID=
say() { echo "nodescreen: $*" | tee -a "$SUM"; }
cleanup() { [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null && kill "$SPID" 2>/dev/null; }   # only our own srun/mnrun PID
trap 'cleanup; exit 2' INT TERM
trap cleanup EXIT
# modules: the same convention as mnrun.sh (Cray SHMEM on aac7, plain rocm elsewhere); NS_MODULES / NS_UNLOAD override
if [ -d /opt/cray/pe/sma ]; then
  case "${AAC7_ROCM:-}" in ""|*/*) ;; *) AAC7_ROCM=rocm/$AAC7_ROCM;; esac
  MODS=${NS_MODULES:-"cray-dsmml cray-openshmemx ${AAC7_ROCM:-rocm/7.2.4}"}; UNL=${NS_UNLOAD-rocm/7.0.3}
else MODS=${NS_MODULES:-rocm}; UNL=${NS_UNLOAD-}; fi
ALLNODES=$(scontrol show hostnames "$(squeue -j "$HOLD" -h -o %N)" 2>/dev/null)
[ -n "$ALLNODES" ] || { echo "nodescreen.sh: job $HOLD has no nodes (squeue)" >&2; exit 2; }
if [ -n "$EXCL" ]; then NODES=$(echo "$ALLNODES" | grep -Fvxf <(echo "$EXCL" | tr , '\n')); else NODES=$ALLNODES; fi
N=$(echo "$NODES" | grep -c .); [ "$N" -ge 1 ] || { echo "nodescreen.sh: no node left" >&2; exit 2; }
say "hold $HOLD, $N node(s), local ${DIGL} digits + cold ${COLD} GB, mn ${DIGM} digits/node, $(TZ=America/New_York date '+%F %H:%M:%S %Z')"
NODELIST=$(echo "$NODES" | paste -sd,)

# ---- the per-node script of the local stage (written once, run by every node; output to local/<host>.log) ----
cat > "$OUT/node_local.sh" <<'NODE'
#!/bin/bash
# node_local.sh <ecalc dir> <cold gb> <digits> <timeout s>  -- runs on one node; lines "NS ..." are the machine-readable results
ED=$1; COLD=$2; DIG=$3; TMO=$4; H=$(hostname -s)
[ -n "${NS_UNLOAD:-}" ] && module unload $NS_UNLOAD >/dev/null 2>&1; module load $NS_MODULES >/dev/null 2>&1
now() { date +%s.%N; }
echo "NS host $H"
a=$(now); timeout "$TMO" "$ED/tests/t_nodechk" 1 3 "$COLD" 2>&1 | grep -a '^NODECHK'; rc=${PIPESTATUS[0]}; b=$(now)
echo "NS step nodechk rc=$rc wall=$(awk -v a=$a -v b=$b 'BEGIN{printf "%.2f", b-a}')"
OF=${TMPDIR:-/tmp}/ns_${H}_$$.out
if [ "$DIG" != 0 ]; then
a=$(now); timeout "$TMO" env ECALC_INIT_TL=1 ECALC_VERBOSE=2 "$ED/ecalc" "$DIG" "$OF" 2>&1 | grep -a -E '^tl |init: |RESULT|VERIFY|total|ERROR|rror'; rc=${PIPESTATUS[0]}; b=$(now)
rm -f "$OF" "$OF".part* 2>/dev/null
echo "NS step ecalc rc=$rc wall=$(awk -v a=$a -v b=$b 'BEGIN{printf "%.2f", b-a}')"
fi
echo "NS done $H"
NODE
chmod +x "$OUT/node_local.sh"

if [ "$DO_LOCAL" = 1 ]; then
  rm -f "$OUT"/local/*.log
  S=$(date +%s.%N)
  timeout -s TERM "$((TMO_L * 2 + 30))" srun --jobid="$HOLD" --export=ALL,NS_MODULES="$MODS",NS_UNLOAD="$UNL" -N"$N" --ntasks="$N" --ntasks-per-node=1 --gpus-per-node=4 --mem=0 -c1 --overlap \
      --nodelist="$NODELIST" --output="$OUT/local/%N.log" --error="$OUT/local/%N.err" bash -l "$OUT/node_local.sh" "$E" "$COLD" "$DIGL" "$TMO_L" < /dev/null &
  SPID=$!; wait "$SPID"; RC_L=$?; SPID=
  W_L=$(awk -v a="$S" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
  say "local stage: wall ${W_L} s, srun rc ${RC_L}, $(ls "$OUT"/local/*.log 2>/dev/null | wc -l) of $N node logs"
fi
echo "$NODES" > "$OUT/nodes.txt"

# ---- the tabulation / rule (also decides which nodes the mn stage keeps) ----
TAB="python3 -I $HERE/nodescreen_tab.py"
KEEP=$($TAB --out "$OUT" --nodes "$OUT/nodes.txt" --thr "$THR" --min-excess "$MINEX" --min-avail-gb "$MINAV" --stage local --quiet 2>/dev/null | sed -n 's/^KEEP //p')
[ "$DO_LOCAL" = 0 ] && KEEP=$NODELIST
NK=$(echo "$KEEP" | tr , '\n' | grep -c .)

if [ "$DO_MN" = 1 ] && [ "$NK" -ge 2 ]; then
  DM=$((DIGM * NK)); S=$(date +%s.%N)
  ( cd "$E" && export SLURM_LABELIO=1 SLURM_JOB_ID="$HOLD" MNRUN_NODELIST="$KEEP" MNRUN_LABEL=1 MNRUN_MODULES="$MODS" MNRUN_UNLOAD="$UNL"
    exec timeout -s TERM "$TMO_M" ./mnrun.sh "$NK" env COMM_TRANSPORT=shmem RNS_DIST_CACHE_FIT=1 ECALC_VERBOSE=2 ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 "$E/ecalc" "$DM" "${TMPDIR:-/tmp}/ns_mn_$$.out" ) > "$OUT/mn.log" 2>&1 < /dev/null &
  SPID=$!; wait "$SPID"; RC_M=$?; SPID=
  rm -f "${TMPDIR:-/tmp}"/ns_mn_$$.out* 2>/dev/null
  W_M=$(awk -v a="$S" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
  echo "NS_MN nodes=$NK digits=$DM rc=$RC_M wall=$W_M" > "$OUT/mn.meta"
  say "mn stage: $NK node(s), $DM digits, wall ${W_M} s, rc ${RC_M}"
elif [ "$DO_MN" = 1 ]; then say "mn stage skipped (fewer than 2 nodes kept by the local stage)"; fi

$TAB --out "$OUT" --nodes "$OUT/nodes.txt" --thr "$THR" --min-excess "$MINEX" --min-avail-gb "$MINAV" --initchk "$HERE/initchk.py" ${FLAGF:+--flagged-file "$FLAGF"}
RC=$?
say "total wall $(awk -v a="$T0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}') s; table $OUT/screen_table.txt; EXCLUDE $OUT/EXCLUDE ($(grep -c . "$OUT/EXCLUDE" 2>/dev/null) host(s))"
exit $RC
