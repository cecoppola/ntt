#!/bin/bash
# s0_check.sh - stage 0, no allocation needed: is the copy intact, are the tools and modules there, what does the queue look like.
#   s0_check.sh [--dry-run]
# Checks: sha256sum -c remote/MANIFEST.sha256 (missing / changed files, first 20 listed); python3, cc/gcc, hipcc/amdclang, make, sbatch,
# salloc, srun, sinfo, squeue, module; `module avail` matches for rocm, cray-shmem/openshmemx, dsmml, libfabric, gmp; sinfo partitions
# and node states.  Verdict: FAIL when the copy differs or python3/make/sbatch/srun are missing, PARTIAL when only optional pieces are
# missing (compilers and modules are found again on a compute node by s1/s2), else PASS.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
while [ $# -gt 0 ]; do case $1 in --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "s0_check.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
rl_init s0
bad=0; warn=0
if [ "$DRY" = 1 ]; then
    rl_would "(cd $R && sha256sum -c remote/MANIFEST.sha256)"; rl_would "command -v python3 cc gcc hipcc amdclang make sbatch salloc srun sinfo squeue module"
    rl_would "module avail rocm / cray-shmem / cray-openshmemx / cray-dsmml / libfabric / gmp"; rl_would "sinfo -h -o '%P %a %l %D %t'"
    rl_finish SKIP "$HERE/s0_check.sh"
fi
# ---- the copy
rl_say "manifest check"
if [ -f "$R/remote/MANIFEST.sha256" ]; then
    ( cd "$R" && timeout 120 sha256sum -c remote/MANIFEST.sha256 > "$RUN/raw/manifest.txt" 2>&1 ); mrc=$?
    nfiles=$(grep -c . "$R/remote/MANIFEST.sha256"); nchg=$(grep -c ': FAILED$' "$RUN/raw/manifest.txt"); nmiss=$(grep -c 'No such file' "$RUN/raw/manifest.txt")
    nchg=$((nchg - nmiss)); [ $nchg -lt 0 ] && nchg=0
    rl_kv manifest_files "$nfiles"; rl_kv manifest_changed "$nchg"; rl_kv manifest_missing "$nmiss"
    if [ $mrc -ne 0 ]; then bad=1; grep -E ': FAILED' "$RUN/raw/manifest.txt" | head -n 20 | sed 's/^/  differs: /' >> "$RUN/summary.body"; fi
else rl_kv manifest "MISSING remote/MANIFEST.sha256"; bad=1; fi
rl_kv repo "$R"; rl_kv host "$(hostname -s 2>/dev/null)"; rl_kv user "$(rl_user)"; rl_kv site "$REMOTE_SITE"
# ---- tools
tools=""; miss_req=""; miss_opt=""
for t in python3 make sbatch srun sinfo squeue salloc scontrol; do
    if rl_have "$t"; then tools="$tools $t"; else case $t in salloc) miss_opt="$miss_opt $t";; *) miss_req="$miss_req $t";; esac; fi
done
for t in cc gcc hipcc amdclang module; do
    if rl_have "$t" || { [ "$t" = module ] && rl_module_init; }; then tools="$tools $t"; else miss_opt="$miss_opt $t"; fi
done
rl_kv tools_found "${tools# }"; [ -n "$miss_req" ] && { rl_kv tools_missing_required "${miss_req# }"; bad=1; }; [ -n "$miss_opt" ] && { rl_kv tools_missing_optional "${miss_opt# }"; warn=1; }
rl_have python3 && rl_kv python3 "$(python3 -V 2>&1)"
rl_have hipcc && rl_kv hipcc "$(rl_tmo 20 hipcc --version 2>&1 | head -1 | cut -c1-80)"
rl_have gcc && rl_kv gcc "$(gcc --version 2>&1 | head -1 | cut -c1-60)"
rl_have sinfo && rl_kv slurm "$(rl_tmo 20 sinfo -V 2>&1 | head -1)"
# ---- modules
rl_say "module avail"
if rl_module_init; then
    rl_tmo 90 bash -lc 'module -t avail 2>&1' > "$RUN/raw/module_avail.txt" 2>&1 < /dev/null || rl_tmo 90 bash -c 'module -t avail 2>&1' > "$RUN/raw/module_avail.txt" 2>&1 < /dev/null
    for k in rocm cray-shmem cray-openshmemx cray-dsmml libfabric gmp; do
        m=$(grep -a -i -E "^$k(/|$)|/$k(/|$)" "$RUN/raw/module_avail.txt" | grep -a -v ':$' | sort -V | uniq | tail -n 6 | paste -sd' ' | cut -c1-160)
        rl_kv "module.$k" "${m:-none}"; [ -z "$m" ] && warn=1
    done
    lm=$(rl_tmo 20 bash -lc 'module -t list 2>&1' < /dev/null | grep -v ':$' | paste -sd' ' | cut -c1-200); rl_kv modules_loaded "${lm:-none}"
else rl_kv modules "no module command in this shell"; warn=1; fi
[ -f /usr/include/gmp.h ] && rl_kv gmp_h /usr/include/gmp.h || rl_kv gmp_h "none in /usr/include (GMP_HOME or a GMP build is needed; s2 handles it)"
# ---- queue
if rl_have sinfo; then
    rl_say "sinfo"
    rl_tmo 30 sinfo -h -o '%P|%a|%l|%D|%t' > "$RUN/raw/sinfo.txt" 2>&1
    rl_line "partitions (name avail timelimit nodes state), up to 20 rows:"
    head -n 20 "$RUN/raw/sinfo.txt" | awk -F'|' '{ printf "  %-28s %-5s %-12s %6s %s\n", $1, $2, $3, $4, $5 }' >> "$RUN/summary.body"
    rl_kv nodes_idle "$(awk -F'|' '$5 ~ /^idle/ { n += $4 } END { print n + 0 }' "$RUN/raw/sinfo.txt")"
    rl_kv nodes_total "$(awk -F'|' '{ n += $4 } END { print n + 0 }' "$RUN/raw/sinfo.txt")"
fi
if [ $bad = 1 ]; then rl_errfiles "$RUN/raw/manifest.txt"; rl_finish FAIL "the copy or required tools are not in order" "fix the listed items (recopy the folder if files differ), then $HERE/s0_check.sh"; fi
rl_finish "$([ $warn = 1 ] && echo PARTIAL || echo PASS)" "$HERE/alloc.sh --nodes 1 --time 1:00:00 && $HERE/s1_facts.sh"
