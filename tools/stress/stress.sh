#!/bin/bash
# tools/stress/stress.sh - the pool/CRT stress harness (results/STRESS.md): factorial arms of repeated ecalc runs inside one Slurm hold on aac7.
#
# USAGE  tools/stress/stress.sh --nodes N --digits D --reps R --out DIR [--hold J] [--arms "k0s0d0r0 k1s1d1r1 ..."] [--ref auto|none|FILE]
#                               [--max-min M] [--nic] [--force-n n] [--chunk-gb g] [--round-mb m] [--timeout SEC]
#   An arm is four letters: k0|k1 = DB_POOL_FORCE_REMAP off / on (n = --force-n, default 3, with DB_POOL_VMM_CHUNK_GB = --chunk-gb, default 0.0625, so that the pool has
#   many small chunks to move); s0|s1 = DB_FREE_SYNC 0 / 1; d0|d1 = diagnostic words off / on (P24_CHECK=2 COMM_SEQ_CHECK=1 XSHMEM_ROUND_STATS=1);
#   r0|r1 = exchange rounds default / tiny (XSHMEM_ROUND_MB = --round-mb, default 2: at 8 PEs a pair then moves in chunks of about 0.3 MiB).  Default: all 16 arms.
#   Every run: DB_POOL_VERBOSE=1 (prints the remap counts at the end), the aac7 launch line, ECALC_SEGV_TRACE=1.  Runs go round by round (rep 1 of every arm, then rep 2 ...),
#   so that a cut-off run still covers all arms.  Per run one line in OUT/runs.tsv: arm rep rc verify crtflag p24flags remaps forced digits wall total.
#   Any anomaly (rc != 0, no VERIFY OK, a P24 flag or CRT flag, digits DIFFERS): the full log is kept in OUT/keep/ and the arm's further repetitions are skipped.
#   --ref auto: the sha256 of the first D digits of the 1e11 reference (up to 1e11 digits; computed once, cached in the stress folder) against the sha256 of each run's unpacked digits;
#   none: no digit file is written (VERIFY and flags only); FILE: a file holding the expected sha256.
#   --nic: around the first run, node snapshots before and after (remote/run/nodesnap.sh) and the kit's NIC delta (rl_nicdiff of remote/run/lib.sh) -> OUT/nic.txt.
#   Writes OUT/summary.md (tools/stress/summarize.py) and the marker OUT/DONE (always, last); removes the digit files.  STOP file in OUT = stop after the current run.
#   Environment overrides: STRESS_CACHE = folder of the cached reference hashes (default: the folder `stress` in the home directory); ECALC_REF = the reference digits
#   file, ASCII "2." + digits, at least D + 3 bytes (default: the file `ref/e_1e11.out` in the home directory).
# Never put credentials or host names in this file.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); E=$ROOT/ecalc
NODES=1 DIGITS= REPS=1 OUT= HOLD=${SLURM_JOB_ID:-} ARMS= REF=auto MAXMIN=100000 NIC=0 FN=3 CG=0.0625 RMB=2 TMO=1500
while [ $# -gt 0 ]; do
  case $1 in
    --nodes) NODES=$2; shift;; --digits) DIGITS=$2; shift;; --reps) REPS=$2; shift;; --out) OUT=$2; shift;; --hold) HOLD=$2; shift;; --arms) ARMS=$2; shift;;
    --ref) REF=$2; shift;; --max-min) MAXMIN=$2; shift;; --nic) NIC=1;; --force-n) FN=$2; shift;; --chunk-gb) CG=$2; shift;; --round-mb) RMB=$2; shift;; --timeout) TMO=$2; shift;;
    -h|--help) sed -n '2,/^# Never put/p' "$0" | cut -c3-; exit 0;;
    *) echo "stress.sh: unknown option $1" >&2; exit 2;;
  esac; shift
done
[ -n "$DIGITS" ] && [ -n "$OUT" ] && [ -n "$HOLD" ] || { echo "stress.sh: --digits, --out and --hold (or SLURM_JOB_ID) are needed" >&2; exit 2; }
DIG=$(awk -v d="$DIGITS" 'BEGIN { printf "%.0f", d }'); [ "$DIG" -gt 0 ] 2>/dev/null || { echo "stress.sh: bad --digits" >&2; exit 2; }
mkdir -p "$OUT/keep" || exit 2; OUT=$(cd "$OUT" && pwd); rm -f "$OUT/DONE"; T0=$(date +%s)
[ -n "$ARMS" ] || ARMS=$(for k in 0 1; do for s in 0 1; do for d in 0 1; do for r in 0 1; do echo "k${k}s${s}d${d}r${r}"; done; done; done; done)
say() { echo "$(date +%T) stress: $*" | tee -a "$OUT/log.txt"; }
finish() {
  rm -f "$OUT"/e.out "$OUT"/e.out.* 2>/dev/null
  python3 -I "$HERE/summarize.py" "$OUT" > "$OUT/summary.md" 2>&1; cat "$OUT/summary.md" | tee -a "$OUT/log.txt" | head -60
  squeue -s -j "$HOLD" 2>/dev/null | tail -n +2 > "$OUT/steps_left.txt"
  echo "done $(date +%T) elapsed $(( $(date +%s) - T0 )) s" > "$OUT/DONE"
}
trap 'finish; exit 1' INT TERM
cd "$E" || { say "no $E"; finish; exit 2; }
type module >/dev/null 2>&1 || . /usr/share/lmod/lmod/init/bash 2>/dev/null
set +u; [ -f ./aac7env.sh ] && . ./aac7env.sh >/dev/null 2>&1; set -u
LINE=$(python3 "$ROOT/tools/profile_load.py" aac7 --line 2>/dev/null)
[ -x "$E/ecalc" ] || { say "no ecalc binary (build first)"; finish; exit 2; }
# the planner's fit, before node time is used (as the gate does)
python3 "$ROOT/tools/plan_values.py" --fit --env "$LINE ECALC_SEGV_TRACE=1" --ecalc "$E/ecalc" "$DIG" "$NODES" > "$OUT/fit.txt" 2>&1 || { say "memory does not fit: $(grep -m1 -E 'REFUSED|plan_values:' "$OUT/fit.txt" | cut -c1-200)"; finish; exit 2; }
say "nodes $NODES digits $DIG reps $REPS hold $HOLD arms: $(echo $ARMS | wc -w); nodes $(squeue -j "$HOLD" -h -o %N)"
REFDIG=${ECALC_REF:-$HOME/ref/e_1e11.out}
REFSHA=
case $REF in
  none) ;;
  auto) if [ "$DIG" -le 100000000000 ]; then   # sha256 of the reference's first D digits ("2." + digits + newline), cached: reading the 1e11 reference costs minutes
          CACHE=${STRESS_CACHE:-$HOME/stress}; RC=$CACHE/refsha_$DIG; mkdir -p "$CACHE"
          [ -s "$RC" ] || { ( head -c $((DIG + 2)) "$REFDIG"; echo ) | sha256sum | cut -d' ' -f1 > "$RC.tmp" && mv "$RC.tmp" "$RC"; }
          REFSHA=$(cat "$RC"); say "reference sha256 of the first $DIG digits: ${REFSHA:0:16}..."
        fi;;
  *) REFSHA=$(cat "$REF");;   # a file holding the sha256 of the expected ASCII output
esac
unset SLURM_CPUS_PER_TASK SLURM_NTASKS SLURM_NPROCS SLURM_NTASKS_PER_NODE SLURM_TASKS_PER_NODE SLURM_NNODES SLURM_JOB_NUM_NODES SLURM_NODELIST \
      SLURM_JOB_NODELIST SLURM_GPUS_PER_NODE SLURM_TRES_PER_TASK SLURM_MEM_PER_NODE SLURM_MEM_PER_CPU SLURM_JOB_CPUS_PER_NODE SLURM_DISTRIBUTION SLURM_CPU_BIND SLURM_GPUS
[ -f "$OUT/runs.tsv" ] || printf 'arm\trep\trc\tverify\tcrtflag\tp24flags\tremaps\tforced\tdigits\twall_s\ttotal_s\n' > "$OUT/runs.tsv"
armwords() {   # ARM -> the arm's environment words
  local a=$1 w=
  case $a in *k1*) w="$w DB_POOL_FORCE_REMAP=$FN DB_POOL_VMM_CHUNK_GB=$CG";; esac
  case $a in *s1*) w="$w DB_FREE_SYNC=1";; esac
  case $a in *d1*) w="$w P24_CHECK=2 COMM_SEQ_CHECK=1 XSHMEM_ROUND_STATS=1";; esac
  case $a in *r1*) w="$w XSHMEM_ROUND_MB=$RMB";; esac
  echo $w
}
snap() {   # TAG: one nodesnap.sh task per node of the hold (no -w: the hold holds exactly our nodes) -> OUT/nic/snap/<tag>/<host>.txt
  local d=$OUT/nic/snap/$1; mkdir -p "$d"
  timeout 180 srun --jobid="$HOLD" --overlap --mem=0 --nodes="$NODES" --ntasks="$NODES" --ntasks-per-node=1 --time=3 --export=ALL \
      -o "$d/%N.txt" -e "$d/%N.err" bash "$ROOT/remote/run/nodesnap.sh" ${2:-} > /dev/null 2>&1 < /dev/null
  say "snapshot $1: $(ls "$d"/*.txt 2>/dev/null | wc -l) of $NODES node file(s)"
}
nicreport() {  # the kit's NIC delta (remote/run/lib.sh rl_nicdiff) of the snapshots -> OUT/nic.txt
  ( export REMOTE_OUT=$OUT/nic; RUN=$OUT/nic; set +u; . "$ROOT/remote/run/lib.sh" 2>/dev/null; RUN=$OUT/nic; rl_nicdiff
    echo "nic_nodes=$NIC_NODES tx_GB_per_node=$NIC_TX_GB rx_GB_per_node=$NIC_RX_GB error_like=$NIC_ERR paused=$NIC_PAUSE (measured; bytes estimated from the packet-size buckets, mean per node)"
    echo "nic_top5=${NIC_TOP:-none}"
    [ -f "$OUT/nic/snap/nic_summary.csv" ] && { head -1 "$OUT/nic/snap/nic_summary.csv"; sort -t, -k3,3nr "$OUT/nic/snap/nic_summary.csv" | grep -v '^host' | head -6; } ) > "$OUT/nic.txt" 2>&1
  say "NIC delta: $(head -1 "$OUT/nic.txt")"
}
declare -A STOPPED
nic_done=0
for rep in $(seq 1 "$REPS"); do
  for arm in $ARMS; do
    [ -e "$OUT/STOP" ] && { say "STOP file: ending"; break 2; }
    [ $(( ($(date +%s) - T0) / 60 )) -ge "$MAXMIN" ] && { say "time budget $MAXMIN min reached: ending"; break 2; }
    [ -n "${STOPPED[$arm]:-}" ] && continue
    W=$(armwords "$arm"); OF=$OUT/e.out; RL=$OUT/run_$arm.$rep.log; rm -f "$OF" "$OF".part*
    OFARG="$OF"; [ -n "$REFSHA" ] || OFARG=
    dosnap=0; [ "$NIC" = 1 ] && [ $nic_done = 0 ] && [ "$NODES" -gt 1 ] && dosnap=1
    [ $dosnap = 1 ] && snap before static
    t1=$(date +%s)
    SLURM_JOB_ID=$HOLD MNRUN_NODES=$NODES MNRUN_LABEL=1 timeout -k 30 "$TMO" ./mnrun.sh "$NODES" env $LINE DB_POOL_VERBOSE=1 $W ECALC_SEGV_TRACE=1 ./ecalc "$DIG" $OFARG > "$RL" 2>&1 < /dev/null
    rc=$?; wall=$(( $(date +%s) - t1 ))
    if [ $dosnap = 1 ]; then snap after; nicreport; nic_done=1; fi
    PL=$RL.plain; sed -E 's/^ *[0-9]+: //' "$RL" > "$PL"
    ver=none; grep -aqE 'VERIFY OK' "$PL" && ver=OK; grep -aqE 'VERIFY (FAIL|MISMATCH|ERROR)' "$PL" && ver=FAIL
    crt=0; grep -aq 'P24 CRT flag' "$PL" && crt=1
    p24=$(grep -a '^P24_CHECK:' "$PL" | sed -E 's/.* ([0-9]+) flags.*/\1/' | awk '{ s += $1 } END { print s + 0 }')
    rem=$(grep -a 'VMM arena released' "$PL" | sed -E 's/.*\(([0-9]+) remaps of .*/\1/' | awk '{ s += $1 } END { print s + 0 }')
    frc=$(grep -a 'VMM arena released' "$PL" | sed -E 's/.* ([0-9]+) of them forced.*/\1/' | awk '{ s += $1 } END { print s + 0 }')
    tot=$(grep -a -m1 '^total' "$PL" | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); tot=${tot:-NA}
    dg=skipped
    if [ $rc = 0 ] && [ -n "$REFSHA" ]; then   # the run's digits unpacked and hashed against the reference's hash (no read of the reference)
      P=$(ls "$OF".part* 2>/dev/null | sort); [ -n "$P" ] || P=$OF
      if [ "$(head -c 8 "$(echo "$P" | head -1)" 2>/dev/null)" = ECPACK18 ]; then h=$("$ROOT/tools/unpack_digits" -q $P 2>/dev/null | sha256sum | cut -d' ' -f1); else h=$(cat $P | sha256sum | cut -d' ' -f1); fi
      [ "$h" = "$REFSHA" ] && dg=identical || dg=DIFFERS
    elif [ $rc = 0 ]; then dg=nofile; fi
    rm -f "$OF" "$OF".part*
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$rep" "$rc" "$ver" "$crt" "$p24" "$rem" "$frc" "$dg" "$wall" "$tot" >> "$OUT/runs.tsv"
    say "$arm rep $rep: rc $rc verify $ver crtflag $crt p24flags $p24 remaps $rem (forced $frc) digits $dg wall ${wall}s total $tot"
    if [ "$rc" != 0 ] || [ "$ver" != OK ] || [ "$crt" != 0 ] || [ "$p24" != 0 ] || { [ "$dg" != identical ] && [ "$dg" != nofile ]; }; then
      cp "$RL" "$OUT/keep/run_$arm.$rep.log"; STOPPED[$arm]=1; say "ANOMALY in arm $arm rep $rep: log kept in keep/; the arm's repetitions stop"
    fi
    rm -f "$RL" "$PL"
  done
done
finish
exit 0
