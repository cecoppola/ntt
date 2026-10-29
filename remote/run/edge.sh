#!/bin/bash
# edge.sh - the node's host-memory edge (TQ05): t_edge (built by s2_build.sh, ecalc/tests/t_edge.c) in its `host` form (hipHostMalloc, pinned, the
# record pools' form) and its `malloc` form (the kernel's overcommit edge) on ONE node of the allocation, alone (nothing else runs in a P1 chain at this
# point).  Measures the real GPU edge (REMOTE_DEVICE_GB) and tells whether pinned host memory beyond the hipMalloc edge is available (the record uses about 380 GB of 515 GB).
#   edge.sh [--jobid J] [--pin GB] [--cap GB] [--tmo SEC] [--dry-run]
# Four t_edge forms in a row (total about 5 min, elapsed printed; skips cleanly, verdict SKIP, when t_edge is not built):
#   dev    hipMalloc round-robin over the 4 APUs in 2 GB chunks, all held at once, touched (40 s): dev_edge_gb (per node), device_edge_apu_gb (min over the 4 APUs)
#   vmm    the same with hipMemCreate/Map (the form ecalc's pools use, 40 s): vmm_edge_apu_gb
#          REMOTE_DEVICE_GB = 4 x (min(device, vmm per-APU edge) - 2 GB margin) is then kept in site.env (planner ECALC_DEVICE_GB and the stair memory rule use it;
#          unset = the 373.44 harness figure of 10-06, superseded by this measurement; not written when the step failed or gave under 60 GB per APU)
#   pin    holds --pin GB (default 24) of pinned hipHostMalloc per APU (NUMA-local), then the same hipMalloc fill (80 s): pin_dev_edge_gb.
#          pinned_counts_against_edge=yes when the hipMalloc edge dropped by at least half of what was pinned, no when it dropped by less than a fifth,
#          unclear in between, unknown when the pinning did not reach half of the request.  Pinned allocation is slow on a used node (0.8-4.8 GiB/s).
#   host   pinned host memory per node (hipHostMalloc, NumaUser, 8 GB steps, cap --cap 440 GB, --tmo 150 s): host_edge_gb (`+` cap reached, `timeout` = lower bound)
#   malloc plain malloc + touch (30 s): malloc_edge_gb (information only)
# The record uses about 380 GB of the 515.7 GB MemAvailable.  Keys: dev_edge_gb pin_requested_gb_per_apu pin_dev_edge_gb pinned_counts_against_edge host_edge_gb
# device_edge_apu_gb vmm_edge_apu_gb remote_device_gb malloc_edge_gb mem_available_start_gb edge_elapsed_s edge_note.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
JOB= STEP=8 CAP=440 TMO=150 PIN=24
while [ $# -gt 0 ]; do case $1 in
  --jobid) JOB=${2:?}; shift;; --step) STEP=${2:?}; shift;; --pin) PIN=${2:?}; shift;; --cap) CAP=${2:?}; shift;; --tmo) TMO=${2:?}; shift;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "edge.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_isnum "$STEP" && rl_isnum "$CAP" && rl_isnum "$PIN" && rl_posint "$TMO" || { echo "edge.sh: --step GB, --cap GB numeric, --tmo SEC a positive integer" >&2; exit 2; }
rl_init edge
bin=$(rl_bin 1); [ -n "$bin" ] || bin=$(rl_bin 2); TE=$(dirname "${bin:-<build>/ecalc/ecalc}")/tests/t_edge
if [ "$DRY" = 1 ]; then rl_would "srun on one node of the job: $TE dev 2 440; $TE vmm 2 440; $TE pin $PIN 2 440; $TE host $STEP $CAP (timeout $TMO s); $TE malloc $STEP $CAP (30 s)"; rl_finish SKIP "$HERE/edge.sh"; fi
[ -n "$bin" ] && [ -x "$TE" ] || { rl_kv dev_edge_gb NA; rl_kv pinned_counts_against_edge unknown; rl_kv host_edge_gb NA; rl_kv malloc_edge_gb NA; rl_kv edge_note "t_edge not built (s2_build.sh builds it): skipped"; rl_finish SKIP "$HERE/s2_build.sh --path 1"; }
rl_conf; rl_resolve_job || rl_die "no allocation: run alloc.sh first (or pass --jobid J)"
rl_need_running "$JOB"
HOST=$(rl_usable_hosts "$JOB" | head -1); [ -n "$HOST" ] || rl_die "no usable node in job $JOB"
MODS=$(rl_mods 1); UNL=$(rl_unload)
rl_kv job "$JOB"; rl_kv edge_host "$HOST (one exclusive node: nothing else of this session runs now)"
form_run() {   # FORM TIMEOUT ARGS... -> $RUN/raw/edge_FORM.out
    local f=$1 to=$2; shift 2
    # shellcheck disable=SC2016
    rl_tmo "$to" srun --jobid="$JOB" --overlap --mem=0 -N1 -n1 -w "$HOST" --export=ALL,EDGE_MODS="$MODS",EDGE_UNL="$UNL" bash -c '
        type module >/dev/null 2>&1 || for i in /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /usr/share/Modules/init/bash /etc/profile.d/modules.sh; do [ -f "$i" ] && . "$i" && break; done
        [ -n "$EDGE_UNL" ] && module unload $EDGE_UNL >/dev/null 2>&1; [ -n "$EDGE_MODS" ] && module load $EDGE_MODS >/dev/null 2>&1
        exec "$@"' _ "$TE" "$f" "$@" > "$RUN/raw/edge_$f.out" 2>&1 < /dev/null
    return $?
}
res() {   # FORM RC -> prints "GB[+]" or "GB (timeout, lower bound)" or NA
    local f=$1 r=$2 o="$RUN/raw/edge_$1.out" g
    g=$(sed -n 's/^t_edge: *\([0-9.]*\) GB committed in.*/\1/p' "$o" | tail -1)
    if grep -aq "the edge in form $f:" "$o"; then g=$(sed -n "s/.*the edge in form $f: \([0-9.]*\) GB committed.*/\1/p" "$o" | tail -1)
        if grep -aq " $f failed\|failed at\|touch failed" "$o"; then echo "$g"; else echo "${g}+"; fi
    elif [ -n "$g" ]; then case $r in 124|137) echo "$g (timeout, lower bound)";; *) echo "$g (killed rc $r)";; esac
    else echo NA; fi
}
v=PASS; E0=$SECONDS
pr() {   # FORM OUT-PATTERN-SED -> value
    sed -n "$2" "$RUN/raw/edge_$1.out" | tail -1
}
apu() {   # FORM -> the per-APU edge (min over the APUs) from the t_edge line
    sed -n 's/.*per-APU edge (min over [0-9]* APUs, all held at once): \([0-9.]*\) GB.*/\1/p' "$RUN/raw/edge_$1.out" | tail -1
}
rl_say "t_edge dev on $HOST (<= 40 s)"; form_run dev 40 2 440; DEV=$(pr dev 's/.*the edge in form dev: \([0-9.]*\) GB.*/\1/p'); rl_kv dev_edge_gb "${DEV:-NA} (hipMalloc, all 4 APUs, nothing pinned)"
DAPU=$(apu dev); rl_kv device_edge_apu_gb "${DAPU:-NA} (min over the 4 APUs, hipMalloc)"
rl_say "t_edge vmm on $HOST (<= 40 s)"; form_run vmm 40 2 440; VAPU=$(apu vmm); rl_kv vmm_edge_apu_gb "${VAPU:-NA} (min over the 4 APUs, hipMemCreate)"
RDG=$(awk -v a="${DAPU:-}" -v b="${VAPU:-}" 'BEGIN { m = ""; if (a != "") m = a + 0; if (b != "" && (m == "" || b + 0 < m)) m = b + 0; if (m == "" || m < 62) { print ""; exit } printf "%.2f", 4 * (m - 2) }')
if [ -n "$RDG" ]; then rl_site_set REMOTE_DEVICE_GB "$RDG"; rl_kv remote_device_gb "$RDG (= 4 x (min per-APU edge - 2 GB); kept in site.env; ECALC_DEVICE_GB of the planner and the stair rule)"
else rl_site_unset REMOTE_DEVICE_GB; rl_kv remote_device_gb "unset (the edge step failed or gave under 60 GB per APU: the 373.44 harness figure stays)"; v=PARTIAL; fi
rl_say "t_edge pin $PIN GB per APU, then hipMalloc (<= 80 s)"; form_run pin 80 "$PIN" 2 440
PD=$(pr pin 's/.*pin result: pinned \([0-9.]*\) GB.*hipMalloc edge \([0-9.]*\) GB per node.*/\1 \2/p'); PT=${PD% *}; PE=${PD#* }
rl_kv pin_requested_gb_per_apu "$PIN"; rl_kv pin_dev_edge_gb "${PE:-NA} (pinned ${PT:-NA} GB in total first)"
verd=unknown
if [ -n "$DEV" ] && [ -n "${PE:-}" ] && [ -n "${PT:-}" ]; then verd=$(awk -v b="$DEV" -v p="$PE" -v t="$PT" -v r="$PIN" 'BEGIN { if (t < 0.5 * r * 4) { print "unknown"; exit } d = b - p; if (d >= 0.5 * t) print "yes"; else if (d < 0.2 * t) print "no"; else print "unclear" }'); fi
rl_kv pinned_counts_against_edge "$verd"
for f in host malloc; do
    rl_say "t_edge $f on $HOST"
    t1=$SECONDS; to=$TMO; [ $f = malloc ] && to=30; form_run "$f" "$to" "$STEP" "$CAP"; rc=$?; r=$(res "$f" "$rc"); rl_kv "${f}_edge_gb" "$r (rc $rc, $((SECONDS - t1)) s)"
    [ "$f" = host ] && rl_kv mem_available_start_gb "$(sed -n 's/.*MemAvailable \([0-9.]*\) GB at the start.*/\1/p' "$RUN/raw/edge_host.out" | head -1)"
    case $r in NA) v=PARTIAL;; esac
    rl_errfiles "$RUN/raw/edge_$f.out"
done
rl_kv edge_elapsed_s "$((SECONDS - E0))"
rl_kv edge_note "dev_edge_gb = hipMalloc edge per node without pinned memory (per node; device_edge_apu_gb the per-APU minimum); pin_dev_edge_gb = the same with pinned host memory held per APU; pinned_counts_against_edge decides whether the record's host pools reduce the device edge; host_edge_gb = pinned host memory per node (+ = cap reached, timeout = lower bound); the record uses about 380 GB host"
for f in dev vmm pin; do tail -n 2 "$RUN/raw/edge_$f.out" | cut -c1-170 | while IFS= read -r l; do rl_line "  $f: $l"; done; done
rl_finish "$v" "$HERE/s5_run.sh --nodes 1 --digits 1.6e10 --path 1"
