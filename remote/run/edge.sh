#!/bin/bash
# edge.sh - the node's host-memory edge (TQ05): t_edge (built by s2_build.sh, ecalc/tests/t_edge.c) in its `host` form (hipHostMalloc, pinned, the
# record pools' form) and its `malloc` form (the kernel's overcommit edge) on ONE node of the allocation, alone (nothing else runs in a P1 chain at this
# point).  Tells whether pinned host memory beyond the 93.36 GB per APU hipMalloc edge is available (the record uses about 380 GB of 515 GB).
#   edge.sh [--jobid J] [--step GB] [--cap GB] [--tmo SEC] [--dry-run]
# Defaults: 8 GB steps, cap 440 GB, 170 s per form (a touch runs at about 2.7 GB/s on aac7: 440 GB = 165 s; up to 6 min in all, less when a form fails early).  Skips (verdict SKIP) when t_edge is not built.  Summary: host_edge_gb,
# malloc_edge_gb (GB committed and touched before the first failure; `+` = the cap was reached without a failure; `timeout` = still running at the time limit, so a lower bound), mem_available_start_gb, edge_note.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
JOB= STEP=8 CAP=440 TMO=170
while [ $# -gt 0 ]; do case $1 in
  --jobid) JOB=${2:?}; shift;; --step) STEP=${2:?}; shift;; --cap) CAP=${2:?}; shift;; --tmo) TMO=${2:?}; shift;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "edge.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_isnum "$STEP" && rl_isnum "$CAP" && rl_posint "$TMO" || { echo "edge.sh: --step GB, --cap GB numeric, --tmo SEC a positive integer" >&2; exit 2; }
rl_init edge
bin=$(rl_bin 1); [ -n "$bin" ] || bin=$(rl_bin 2); TE=$(dirname "${bin:-<build>/ecalc/ecalc}")/tests/t_edge
if [ "$DRY" = 1 ]; then rl_would "srun on one node of the job: $TE host $STEP $CAP; $TE malloc $STEP $CAP (timeout $TMO s each)"; rl_finish SKIP "$HERE/edge.sh"; fi
[ -n "$bin" ] && [ -x "$TE" ] || { rl_kv host_edge_gb NA; rl_kv malloc_edge_gb NA; rl_kv edge_note "t_edge not built (s2_build.sh builds it): skipped"; rl_finish SKIP "$HERE/s2_build.sh --path 1"; }
rl_conf; rl_resolve_job || rl_die "no allocation: run alloc.sh first (or pass --jobid J)"
rl_need_running "$JOB"
HOST=$(rl_usable_hosts "$JOB" | head -1); [ -n "$HOST" ] || rl_die "no usable node in job $JOB"
MODS=$(rl_mods 1); UNL=$(rl_unload)
rl_kv job "$JOB"; rl_kv edge_host "$HOST (one exclusive node: nothing else of this session runs now)"
form_run() {   # FORM -> $RUN/raw/edge_FORM.out
    local f=$1
    # shellcheck disable=SC2016
    rl_tmo "$TMO" srun --jobid="$JOB" --overlap --mem=0 -N1 -n1 -w "$HOST" --export=ALL,EDGE_MODS="$MODS",EDGE_UNL="$UNL" bash -c '
        type module >/dev/null 2>&1 || for i in /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /usr/share/Modules/init/bash /etc/profile.d/modules.sh; do [ -f "$i" ] && . "$i" && break; done
        [ -n "$EDGE_UNL" ] && module unload $EDGE_UNL >/dev/null 2>&1; [ -n "$EDGE_MODS" ] && module load $EDGE_MODS >/dev/null 2>&1
        exec "$@"' _ "$TE" "$f" "$STEP" "$CAP" > "$RUN/raw/edge_$f.out" 2>&1 < /dev/null
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
v=PASS
for f in host malloc; do
    rl_say "t_edge $f on $HOST (<= $TMO s)"
    t1=$SECONDS; form_run "$f"; rc=$?; r=$(res "$f" "$rc"); rl_kv "${f}_edge_gb" "$r (rc $rc, $((SECONDS - t1)) s)"
    case $r in NA|*killed*) v=PARTIAL;; esac
    [ "$f" = host ] && rl_kv mem_available_start_gb "$(sed -n 's/.*MemAvailable \([0-9.]*\) GB at the start.*/\1/p' "$RUN/raw/edge_host.out" | head -1)"
    rl_errfiles "$RUN/raw/edge_$f.out"
done
ha=$(sed -n 's/^host_edge_gb=\([0-9.]*\).*/\1/p' "$RUN/summary.body" | head -1)
if [ -n "$ha" ]; then rl_kv edge_note "host_edge_gb = pinned host memory in the record pools' form (+ = cap reached, no failure; timeout = lower bound); the hipMalloc edge is 93.36 GB per APU = 373.44 GB per node; the record's host use is about 380 GB (read host_peak_gb of the staircase rows)"; fi
tail -n 2 "$RUN/raw/edge_host.out" | cut -c1-150 | while IFS= read -r l; do rl_line "  host: $l"; done
tail -n 2 "$RUN/raw/edge_malloc.out" | cut -c1-150 | while IFS= read -r l; do rl_line "  malloc: $l"; done
rl_finish "$v" "$HERE/s5_run.sh --nodes 1 --digits 1.6e10 --path 1"
