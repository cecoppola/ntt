#!/bin/bash
# s1_facts.sh - stage 1: collect the machine's facts and write the settings later stages use.
#   s1_facts.sh [--jobid J] [--no-node] [--partition P] [--time H:MM:SS] [--dry-run]
# Login probe (target/probe/probe.sh --only login), then the node probe inside a one-node allocation: --jobid J, else REMOTE_JOB from
# site.env (any node of it), else a short ONE-node job of its own (cancelled at the end; only that job).  Then target/probe/gen_conf.py
# writes target/config/target.conf (aac7 rehearsal: $RUN/target.conf.aac7 instead) and site.env gets REMOTE_PARTITION, REMOTE_MODULES_P1/P2,
# REMOTE_UNLOAD, GMP_HOME (REMOTE_JOB is kept).  Summary: gpu count, NICs per node and per APU, MemAvailable, ROCm / SMA / libfabric
# versions, userfaultfd, hugetlb/THP, SHMEM test rc per case, the proposed settings and the keys still open.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
JOB= NONODE=0 PART= TLIM=0:30:00
while [ $# -gt 0 ]; do case $1 in
  --jobid) JOB=${2:?}; shift;; --no-node) NONODE=1;; --partition) PART=${2:?}; shift;; --time) TLIM=${2:?}; shift;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "s1_facts.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_init s1
PART=${PART:-${REMOTE_PARTITION:-}}
PROBE=$R/target/probe/probe.sh; GEN=$R/target/probe/gen_conf.py
PW=(); [ "$REMOTE_SITE" = aac7 ] && PW=(--want-sma 11.8)   # the rehearsal site has SMA 11.8, the target 12.1 (probe default)
L=$RUN/login; N=$RUN/node
OWN=; wl="$HERE/s1_facts.sh"
if [ "$DRY" = 1 ]; then
    rl_would "$PROBE --only login --out $L"
    rl_would "(no REMOTE_JOB/--jobid) sbatch -N1 --exclusive --gpus-per-node=4 -t $TLIM -J remote_s1 ${PART:+-p $PART }--wrap 'sleep ...'  # own one-node job, cancelled at the end"
    rl_would "$PROBE --only node --jobid <job> --out $N"
    rl_would "python3 $GEN $L $N -o $R/target/config/target.conf"
    rl_would "site.env <- REMOTE_PARTITION REMOTE_MODULES_P1 REMOTE_MODULES_P2 REMOTE_UNLOAD GMP_HOME"
    rl_finish SKIP "$wl"
fi
cancel_own() { [ -n "$OWN" ] && { rl_tmo 60 scancel "$OWN" >> "$RUN/log.txt" 2>&1; rl_say "cancelled own job $OWN"; OWN=; }; }
# ---- login part
rl_say "login probe (<= 3 min)"
rl_tmo 240 bash "$PROBE" --only login ${PW[@]+"${PW[@]}"} --out "$L" > "$RUN/raw/probe_login.log" 2>&1 < /dev/null; lrc=$?
rl_errfiles "$RUN/raw/probe_login.log"
[ -f "$L/facts.tsv" ] || rl_finish FAIL "login probe produced no facts.tsv (rc $lrc)" "bash $PROBE --only login --out $RUN/login2"
# ---- node part
nrc=NA
if [ $NONODE = 0 ]; then
    rl_resolve_job || JOB=
    if [ -n "$JOB" ] && [ "$(rl_job_state "$JOB")" != RUNNING ]; then rl_say "job $JOB is not running: not used for the node probe"; JOB=; fi
    if [ -z "$JOB" ]; then
        secs=$(echo "$TLIM" | awk -F: '{ print ($1 * 3600 + $2 * 60 + $3) + 30 }')
        EXL=$(rl_exclude_list)
        out=$(rl_tmo 120 sbatch --parsable -N1 --exclusive --gpus-per-node=4 -t "$TLIM" -J remote_s1 --no-requeue -o "$RUN/raw/slurm_s1.out" ${PART:+-p "$PART"} ${EXL:+-x "$EXL"} --wrap "sleep $secs" 2>"$RUN/raw/sbatch.err")
        OWN=${out%%;*}
        if rl_isnum "$OWN"; then
            rl_say "own one-node job $OWN submitted; waiting up to 300 s"
            t0=$SECONDS; while [ "$(rl_job_state "$OWN")" != RUNNING ] && [ $((SECONDS - t0)) -lt 300 ]; do sleep 10; done
            if [ "$(rl_job_state "$OWN")" = RUNNING ]; then JOB=$OWN; else rl_say "own job did not start in 300 s: node probe skipped"; cancel_own; fi
        else rl_say "no one-node job possible: $(head -c 200 "$RUN/raw/sbatch.err")"; OWN=; rl_errfiles "$RUN/raw/sbatch.err"; fi
    fi
    if [ -n "$JOB" ]; then
        PHOST=$(rl_tmo 30 squeue -h -j "$JOB" -o %N 2>/dev/null | head -1)
        rl_say "node probe in job $JOB on ${PHOST:-?} (<= 8 min)"
        rl_tmo 600 bash "$PROBE" --only node ${PW[@]+"${PW[@]}"} --jobid "$JOB" --out "$N" > "$RUN/raw/probe_node.log" 2>&1 < /dev/null; nrc=$?
        rl_errfiles "$RUN/raw/probe_node.log"
        cancel_own
    fi
fi
# ---- target.conf
conf=$R/target/config/target.conf
if [ "$REMOTE_SITE" = aac7 ]; then conf=$RUN/target.conf.aac7; else [ -f "$conf" ] && cp "$conf" "$RUN/target.conf.previous"; fi
rl_say "gen_conf"
args=("$L"); [ -f "$N/facts.tsv" ] && args+=("$N")
rl_tmo 120 python3 -I "$GEN" "${args[@]}" -o "$conf" > "$RUN/raw/gen_conf.txt" 2>&1 < /dev/null; grc=$?
# ---- facts
fact() { local k=$1 v=; [ -f "$N/facts.tsv" ] && v=$(awk -F'\t' -v k="$k" '$1 == k { v = $2 } END { print v }' "$N/facts.tsv"); [ -n "$v" ] || v=$(awk -F'\t' -v k="$k" '$1 == k { v = $2 } END { print v }' "$L/facts.tsv"); echo "${v:-missing}" | cut -c1-120; }
rl_kv login_probe_rc "$lrc"; rl_kv node_probe_rc "$nrc"; rl_kv node_job "${JOB:-none}${OWN:+ (own one-node job, outside the session allocation)}"; rl_kv node_probe_host "${PHOST:-none}"
rl_kv gpu_count "$(fact gpu.count)"; rl_kv gpu_names "$(fact gpu.names)"
rl_kv nics_cxi "$(fact net.cxi_count)"; rl_kv nics_per_apu "$(fact net.nics_per_apu)"
mk=$(fact mem.available_kb); rl_kv mem_available_gb "$(awk -v k="$mk" 'BEGIN { if (k + 0 > 0) printf "%.0f", k / 1048576; else print "missing" }')"
rl_kv cpus "$(fact cpu.total)"; rl_kv rocm "$(fact rocm.version_file)"; rl_kv hipcc "$(fact rocm.hipcc_version)"
rl_kv sma_versions "$(fact lib.sma_versions)"; rl_kv sma_prefix "$(fact lib.sma_prefix)"; rl_kv libfabric "$(fact lib.fi_version)"; rl_kv libfabric_cxi_domains "$(fact lib.fi_cxi_domain_count)"
rl_kv userfaultfd "syscall=$(fact uffd.syscall) sysctl=$(fact uffd.sysctl) fi_monitor_uffd=$(fact lib.fi_monitor.userfaultfd)"
rl_kv thp "enabled=$(fact mem.thp_enabled) defrag=$(fact mem.thp_defrag)"; rl_kv hugetlb "2M_nr=$(fact mem.hugetlb_2m_nr) 2M_free=$(fact mem.hugetlb_2m_free) pages_total=$(fact mem.hugepages_total)"
rl_kv slurm "$(fact slurm.version)"; rl_kv default_partition "$(fact slurm.default_partition)"
shm=$(for f in "$N/facts.tsv" "$L/facts.tsv"; do [ -f "$f" ] && awk -F'\t' '$1 ~ /^shmem\.rc\./ { sub(/^shmem\.rc\./, "", $1); printf "%s=%s ", $1, $2 }' "$f"; done)
rl_kv shmem_test_rc "${shm:-none}"
# ---- proposed settings and open keys
rl_line "proposed settings ($conf):"
for k in TARGET_ROCM TARGET_SMA_MODULE TARGET_DSMML_MODULE MNRUN_MODULES MNRUN_UNLOAD GMP_HOME TARGET_PARTITION ECALC_NODE_GB ECALC_OUT_DIR MNACCEPT_TMP; do
    v=$(grep -a -m1 -E "^$k=" "$conf" 2>/dev/null | sed -E "s/^[^=]*=//; s/[ ]+#.*//" | cut -c1-100); [ -n "$v" ] && rl_line "  $k=$v"
done
open=$(grep -a -E '^=== open keys' "$RUN/raw/gen_conf.txt" | head -1 | sed 's/^=== open keys ([0-9]*): *//'); rl_kv open_keys "${open:-none}"
grep -a -E '^  (NICs per APU|WARNING|SHMEM test|uffd)' "$RUN/raw/gen_conf.txt" | head -n 6 | cut -c1-200 | sed 's/^  /advisory: /' >> "$RUN/summary.body"
# ---- site.env
cv() { grep -a -m1 -E "^$1=" "$conf" 2>/dev/null | sed -E 's/^[^=]*=//; s/[ ]+#.*//; s/^"(.*)"$/\1/' ; }
if [ "$REMOTE_SITE" != aac7 ] && [ -f "$conf" ]; then
    m=$(cv MNRUN_MODULES); u=$(cv MNRUN_UNLOAD); g=$(cv GMP_HOME); p=$(cv TARGET_PARTITION)
    case $m in ''|__FILL__) ;; *) rl_site_set REMOTE_MODULES_P2 "$m"; rl_site_set REMOTE_MODULES_P1 "$(echo "$m" | tr ' ' '\n' | grep -v -E '^cray-(dsmml|openshmemx|shmem)' | paste -sd' ')";; esac
    case $u in __FILL__) ;; *) rl_site_set REMOTE_UNLOAD "$u";; esac
    case $g in ''|__FILL__) ;; *) rl_site_set GMP_HOME "$g";; esac
    case $p in ''|__FILL__) ;; *) rl_site_set REMOTE_PARTITION "$p";; esac
    rl_kv site_env "$SITE_ENV (REMOTE_JOB kept: ${REMOTE_JOB:-none})"
fi
v=PASS
[ "$nrc" = 0 ] || v=PARTIAL
[ "$grc" = 0 ] || { v=FAIL; rl_errfiles "$RUN/raw/gen_conf.txt"; }
case "$(fact gpu.count)" in 4) ;; *) [ "$REMOTE_SITE" = aac7 ] || v=PARTIAL;; esac
echo "$shm" | grep -q -E '=[1-9]' && v=PARTIAL
rl_finish "$v" "$HERE/s2_build.sh --path both --jobs 32"
