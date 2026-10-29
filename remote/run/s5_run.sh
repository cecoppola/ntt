#!/bin/bash
# s5_run.sh - stage 5: one optimized ecalc run, with the checks around it.
#   s5_run.sh --nodes N --digits D --path 1|2 [--env "K=V ..."] [--screen] [--write DIR] [--exclude LIST] [--hosts LIST] [--tmo MIN] [--jobid J]
#             [--fulllog] [--monitor SEC] [--dry-run]
# D may be 64e9, 38.2e12 or a plain integer.  Path 2 adds ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 and uses the path 2 binary by itself; --env adds
# settings words (also --env "").  Order: (1) planner fit check (tools/plan_values.py --fit): on refusal the largest digit count that fits is
# printed (largest_fit_digits=) and the script stops with FAIL; (2) --screen: tools/nodescreen.sh over the allocation, the slow nodes are left
# out of the run and printed as slow_hosts=<list> (usable with --exclude in later calls); (3) the run through ecalc/mnrun.sh; (4) model time
# (ecalc/mn_model.py estimate) and measured/model; (5) VERIFY, the digit hash against the reference when the size has one (needs --write DIR,
# else the last digits), rc.  --write DIR keeps the digit file there (it is large: 0.417 B per digit; not used for timing runs).
# --hosts LIST (comma list of exactly N usable hosts of the allocation; used by arms.sh to run several arms at once in one allocation; not with
# --screen) runs on those hosts instead of the first N.  Summary extras (KEY=VALUE, NA = not printed): layout (PEs, per node, background
# team), init_split (the ECALC_INIT_TL=1 line), shifts (the recip(mn) shifts n/s figure, ECALC_VERBOSE>=1 lines), p24_check (the P24_CHECK
# line), device_peak_apu_gb (largest per-APU in-use figure of the memory table).
# --fulllog (or REMOTE_FULLLOG=1, e.g. exported before stair.sh; off by default) = the exhaustive log for offline analysis: adds the digit-neutral diagnostic words
# (MN_WAIT_STATS=1 ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=1; words already in --env win), writes $RUN/capture/ (launch line, environment without secrets, modules,
# scontrol job, hosts, binary sha256, mnrun's computed launch values), takes a node snapshot before and after the run ($RUN/snap/before|after/<host>.txt: meminfo,
# numastat, amd-smi, all nonzero Slingshot NIC counters; $RUN/snap/nic_summary.csv = the after-minus-before NIC traffic, bytes estimated from the packet size
# buckets) and samples every node every 30 s into $RUN/monitor/<host>.csv.  --monitor SEC sets or (0) switches off the sampler (REMOTE_MONITOR=SEC).  Summary
# keys: fulllog, nic_gb_per_node, nic_error_like.  Nothing here can fail the run; a failed snapshot or sampler is skipped.  collect.sh bundles it all.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
RL_INVOKE="$0 $*"
NN= DIG= P= ENVW= SCREEN=0 WDIR= TMO= JOB= EXCLUDE= HLIST= FL=${REMOTE_FULLLOG:-0} MON=${REMOTE_MONITOR:-}
while [ $# -gt 0 ]; do case $1 in
  --nodes) NN=${2:?}; shift;; --digits) DIG=${2:?}; shift;; --path) P=${2:?}; shift;; --env) ENVW=${2-}; shift;; --screen) SCREEN=1;;
  --write) WDIR=${2:?}; shift;; --exclude) EXCLUDE=${2-}; shift;; --hosts) HLIST=${2:?}; shift;; --tmo) TMO=${2:?}; shift;; --jobid) JOB=${2:?}; shift;; --fulllog) FL=1;; --monitor) MON=${2:?}; shift;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "s5_run.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
[ -n "$NN" ] && [ -n "$DIG" ] && [ -n "$P" ] || { echo "s5_run.sh: --nodes, --digits and --path are required (--help)" >&2; exit 2; }
rl_posint "$NN" && rl_isnum "$DIG" || { echo "s5_run.sh: --nodes a positive integer, --digits numeric (64e9, 38.2e12 or an integer)" >&2; exit 2; }
NN=$((10#$NN))
case $P in 1|2) ;; *) echo "s5_run.sh: --path 1|2" >&2; exit 2;; esac
HLIST=$(echo "$HLIST" | tr -d ' ')
[ -z "$HLIST" ] || [ -z "$(echo "$HLIST" | tr ',' '\n' | sort | uniq -d)" ] || { echo "s5_run.sh: --hosts has duplicate hosts" >&2; exit 2; }
[ -z "$HLIST" ] || ! echo ",$HLIST," | grep -q ',,' || { echo "s5_run.sh: --hosts has a blank host" >&2; exit 2; }
[ -z "$HLIST" ] || [ "$SCREEN" = 0 ] || { echo "s5_run.sh: --hosts and --screen do not combine (the screen covers the whole allocation)" >&2; exit 2; }
[ -z "$HLIST" ] || [ "$(echo "$HLIST" | tr ',' '\n' | grep -c .)" = "$NN" ] || { echo "s5_run.sh: --hosts needs exactly $NN host(s)" >&2; exit 2; }
[ -z "$MON" ] || case $MON in *[!0-9]*) echo "s5_run.sh: --monitor SEC: a number of seconds (0 = off)" >&2; exit 2;; esac
[ "$FL" = 1 ] || FL=0
[ -n "$MON" ] || { [ $FL = 1 ] && MON=30 || MON=0; }; MON=$((10#$MON))
rl_init s5
D=$(rl_digits_site "$DIG" "$NN")
FLW=; [ $FL = 1 ] && FLW=$(rl_fl_words "$ENVW")
words=($ENVW $FLW)
if [ "$DRY" = 1 ]; then
    rl_would "fit: python3 tools/plan_values.py --fit --ecalc <p$P binary> --env \"$([ $P = 2 ] && echo 'ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ')$ENVW\" $D $NN   (refused: bisect for the largest fitting digits)"
    [ -n "$HLIST" ] && rl_would "hosts: $HLIST (this arm's subset of the allocation)"
    [ $SCREEN = 1 ] && rl_would "tools/nodescreen.sh --hold <job> --out $RUN/screen --ecalc-dir <build p$P>/ecalc   -> slow_hosts"
    rl_would "$(rl_cmdline "$P" "$NN" "$D" ${words[@]+"${words[@]}"})${WDIR:+  # digit file in $WDIR}"
    [ $FL = 1 ] && rl_would "fulllog: words ${FLW:-<none new>}; capture/ files; node snapshots before+after (srun --overlap, 1 task per node; nodesnap.sh); sampler every ${MON}s (monitor.sh); snap/nic_summary.csv"
    [ $FL = 0 ] && [ "$MON" -gt 0 ] && rl_would "sampler every ${MON}s (monitor.sh)"
    rl_would "model: python3 ecalc/mn_model.py estimate --g $NN --D $(awk -v d="$D" -v n="$NN" 'BEGIN { printf "%.6g", d / n }')"
    rl_finish SKIP "$HERE/s5_run.sh --nodes $NN --digits $DIG --path $P"
fi
if [ -n "$HLIST" ]; then    # --hosts: the arm's own node subset (every host must be a usable node of the allocation)
    rl_prep "$P" 100000
    for h in $(echo "$HLIST" | tr ',' ' '); do echo "$HOSTS_ALL" | tr ',' '\n' | grep -qxF "$h" || rl_die "--hosts: $h is not a usable node of job $JOB"; done
    HOSTS_ALL=$HLIST; NHOSTS=$NN; export HOSTS=$HLIST
else rl_prep "$P" "$NN"; fi
[ "$NHOSTS" -ge "$NN" ] || rl_die "only $NHOSTS usable nodes in the allocation, $NN asked"
rl_kv path "$P"; rl_kv nodes "$NN"; rl_kv digits "$D"; rl_kv settings "${ENVW:-<defaults>}$([ "$P" = 2 ] && echo ' + ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 (path 2)')"
[ $FL = 1 ] && rl_kv fulllog_words "${FLW:-<none new>}"
[ "$D" != "$(rl_num "$DIG")" ] && rl_kv digits_requested "$DIG (capped for the rehearsal site)"
# ---- (1) fit
rl_say "planner fit check"
rl_fit "$P" "$NN" "$D" ${words[@]+"${words[@]}"}; frc=$?
if [ $frc = 1 ]; then
    rl_errfiles "$RUN/raw/fit_${P}_${NN}_$D.log"
    rl_say "does not fit; searching the largest digit count that fits"
    mx=$(rl_maxfit "$P" "$NN" "$D" ${words[@]+"${words[@]}"})
    rl_kv fit "REFUSED: $FIT_LINE"; rl_kv largest_fit_digits "$mx"
    rl_finish FAIL "does not fit: the largest fitting digit count is $mx" "$HERE/s5_run.sh --nodes $NN --digits $mx --path $P${ENVW:+ --env \"$ENVW\"}"
elif [ $frc = 2 ]; then rl_errfiles "$RUN/raw/fit_${P}_${NN}_$D.log"; rl_finish FAIL "the planner failed (see raw/fit_*.log)" "$HERE/show.sh $RUN errors"; fi
rl_kv fit "FITS: $FIT_LINE"
# ---- (2) screen
if [ $SCREEN = 1 ]; then
    rl_say "node screen (<= 20 min)"
    tree=$(dirname "$(rl_bin "$P")"); [ -x "$tree/tests/t_nodechk" ] || tree=$(dirname "$(rl_bin 1)")
    ex=$(rl_exclude_list)
    nsm=$(rl_mods "$P"); [ -n "$nsm" ] || nsm=$(rl_mods 1)
    NS_MODULES=$nsm NS_UNLOAD=$(rl_unload) rl_tmo 1500 bash "$R/tools/nodescreen.sh" --hold "$JOB" --out "$RUN/screen" --ecalc-dir "$tree" ${ex:+--exclude-hosts "$ex"} ${REMOTE_NICS:+--want-nics "$REMOTE_NICS"} > "$RUN/raw/nodescreen.log" 2>&1 < /dev/null; src=$?
    rl_errfiles "$RUN/raw/nodescreen.log"
    slow=$(grep -a . "$RUN/screen/EXCLUDE" 2>/dev/null | paste -sd,)
    case $src in 0) sw="clean";; 3) sw="slow hosts flagged";; 2) sw="screen could not run";; *) sw="unexpected";; esac; rl_kv screen "$sw (rc $src)"; rl_kv slow_hosts "${slow:-}"
    [ -n "$slow" ] && EXCLUDE=${EXCLUDE:+$EXCLUDE,}$slow
    if [ $src = 2 ]; then rl_kv screen_note "screen failed: nodes not screened"; fi
    if [ -n "${slow:-}" ]; then
        h=$(rl_pick_hosts "$JOB" "$NN") || rl_finish FAIL "after leaving out slow_hosts=$slow fewer than $NN nodes remain" "$HERE/alloc.sh --nodes <more>"
        export HOSTS=$h
    fi
fi
rl_kv hosts "$(echo "${HOSTS:-}" | cut -c1-200)"
# ---- (3) model, run
model=$(rl_model "$NN" "$D")
if [ -z "$TMO" ]; then TMO=$(awk -v m="$model" 'BEGIN { if (m + 0 > 0) printf "%d", m * 3 / 60 + 15; else print 60 }'); fi
export TMO
[ -n "$WDIR" ] && { mkdir -p "$WDIR"; export WRITE=1 KEEP=1 TGT_SCRATCH=$WDIR; }
rl_say "run: path $P, $NN node(s), $D digits (timeout $TMO min; model ${model} s)"
rl_log "$(rl_cmdline "$P" "$NN" "$D" ${words[@]+"${words[@]}"})"
if [ $FL = 1 ]; then rl_capture "$P" "$NN" "$D" ${words[@]+"${words[@]}"}; rl_snap before static; fi
[ "$MON" -gt 0 ] && rl_mon_start "$MON" "$TMO"
rl_ecrun s5_run "$P" "$NN" "$D" ${words[@]+"${words[@]}"}; rc=$?
tag=s5_run
rl_mon_stop
if [ $FL = 1 ]; then rl_snap after; rl_nicdiff; rl_kv nic_gb_per_node "tx ${NIC_TX_GB} rx ${NIC_RX_GB} over ${NIC_NODES} node(s) (estimated from the NIC packet size buckets, mean per node)"; rl_kv nic_error_like "${NIC_ERR} counts, ${NIC_PAUSE} paused counts (mean per node)"; fi
[ $FL = 1 ] && rl_kv fulllog "on: capture/ snap/ ${MON:+monitor/ (every ${MON} s) }in $RUN"
# ---- results
rl_kv rc "$rc"; rl_kv verify "${E_VERIFY:-NONE}"
rl_kv "total_s" "${E_TOTAL:-NA}"
rl_line "phases_s: init=${E_INIT:-NA} bs=${E_BS:-NA} recip=${E_RECIP:-NA} dm=${E_DM:-NA} dc=${E_DC:-NA} T1=${E_T1:-NA} T2=${E_T2:-NA} other=${E_OTHER:-NA}"
rl_kv wall_launch_s "${E_WALL_S:-NA}"
rl_kv model_s "$model (modelled, no digit write)"
rl_kv meas_over_model "$(awk -v t="${E_TOTAL:-0}" -v m="$model" 'BEGIN { if (t + 0 > 0 && m + 0 > 0) printf "%.3f", t / m; else print "NA" }')"
rl_runfacts "$RUN/raw/$tag.log" "$P"; rl_factlines "$NN"
rl_kv device_peak_apu_gb "${E_DEVPEAK_APU_GB:-NA} (measured, per APU, from the mem[...] table rows; NA when this build prints none)"
rl_kv device_plan_apu_gb "${RF_PLANAPU:-NA} (modelled by the planner at the start, per APU)"
[ -n "${E_DEVPEAK_GB:-}" ] && rl_kv device_peak_gb "$E_DEVPEAK_GB (apu max ${E_DEVPEAK_APU_GB:-?})"
[ -n "${E_HOSTPEAK_GB:-}" ] && rl_kv host_peak_gb "$E_HOSTPEAK_GB"
tail=$(rl_digits_tail "$RUN/raw/$tag.log"); rl_kv last_digits "${tail:-none printed}"
if [ -n "${E_HASH:-}" ]; then rl_kv digit_hash "$E_HASH (reference ${E_HASH_REF:-none}: ${E_HASH_MATCH:--})"; else rl_kv digit_hash "not taken (no --write)"; fi
[ -n "${E_CONFIG_HASH:-}" ] && rl_kv config_hash "$E_CONFIG_HASH"
[ -n "$WDIR" ] && rl_kv digit_files "$WDIR"
ok=1; { [ "$rc" = 0 ] && [ "${E_VERIFY:-NONE}" = OK ] && [ "${E_HASH_MATCH:--}" != no ]; } || ok=0
if [ $ok = 0 ]; then rl_errfiles "$RUN/raw/$tag.log" "$RUN/raw/$tag.log.gz"; rl_finish FAIL "rc=$rc verify=${E_VERIFY:-NONE}${E_REFUSED:+ refused=$E_REFUSED}" "$HERE/show.sh $RUN errors"; fi
rl_finish PASS "$HERE/s4_ab.sh --nodes $NN --digits $D --path $P --a \"${ENVW}\" --b \"<variant>\" --rounds 2"
