#!/bin/bash
# collect.sh - ONE bundle of everything the session logged, for the offline analysis.
#   collect.sh [--out DIR] [--paste] [--dry-run]
# Writes $REMOTE_OUT/collect_<timestamp>.tar.gz (--out DIR: there) and collect_<timestamp>.tar.gz.sha256.  Inside (one top directory collect_<timestamp>/):
#   MANIFEST.txt (size and sha256 of every file), README.txt (what each part is), runs/<run dir>/ for EVERY run dir under $REMOTE_OUT in full (summaries, raw logs,
#   planner and fit logs, model output, screens, capture/, snap/, monitor/ ...), site.env, meta/ (kit identity: manifest check and git commit; the s2 binaries'
#   sha256 and ldd; module list; compiler / ROCm / libfabric / SHMEM / Slurm versions; sacct -j J --format=ALL -P for every job id found in the run dirs, scontrol
#   show job while the job is known, scontrol show node and topology, sinfo for the nodes used; digit_files.txt = path, size and printed hash of each digit file).
# NOT in the bundle: the digit files themselves (TBs; $REMOTE_OUT/scratch, *.part*, any other file over 4 GiB, listed in meta/skipped_files.txt).
# Works after the allocation is released (sacct) and skips every tool that is missing.  Run it when no run is in progress (a log still being written is read as it is).
# --paste also prints a digest of at most 60 lines (one line per run) and the bundle line, for the browser agent.  --dry-run lists what would be collected.
# The bundle holds host names and paths: it is private; the user moves it themselves; it never goes to GitHub.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
OUTD= PASTE=0; DRY=0
while [ $# -gt 0 ]; do case $1 in
  --out) OUTD=${2:?}; shift;; --paste) PASTE=1;; --dry-run) DRY=1;; -h|--help) rl_help "$0"; exit 0;; *) echo "collect.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
[ -d "$REMOTE_OUT" ] || { echo "collect.sh: no $REMOTE_OUT (REMOTE_OUT)" >&2; exit 2; }
[ -n "$OUTD" ] || OUTD=$REMOTE_OUT
rl_site_load
kvf() { grep -a -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2-; }
TS=$(date +%Y%m%d-%H%M%S); NAME=collect_$TS; BUNDLE=$OUTD/$NAME.tar.gz
RUNS=$(cd "$REMOTE_OUT" && ls -d */ 2>/dev/null | tr -d / | grep -v -E '^(collect_|scratch$|latest$)' | grep -E '^[a-z0-9]+_[0-9]{8}-[0-9]{6}' | sort)
JOBS=$(for r in $RUNS; do grep -a -h -o -E '^(job|hold_job|jobid)=[0-9]+' "$REMOTE_OUT/$r/summary.txt" 2>/dev/null; grep -a -h -o -E 'SLURM_JOB_ID=[0-9]+' "$REMOTE_OUT/$r/log.txt" "$REMOTE_OUT/$r/capture/cmdline.txt" 2>/dev/null; done | grep -o '[0-9]*$' | sort -un)
[ -z "${REMOTE_JOB:-}" ] || JOBS=$(printf '%s\n%s\n' "$JOBS" "$REMOTE_JOB" | grep . | sort -un)
HOSTL=$(for r in $RUNS; do cat "$REMOTE_OUT/$r/capture/hosts.txt" 2>/dev/null; done | grep . | sort -u | paste -sd,)
if [ "$DRY" = 1 ]; then
    echo "collect.sh dry-run: would write $BUNDLE (+ .sha256)"
    echo "  runs under $REMOTE_OUT: $(echo $RUNS | wc -w)"; for r in $RUNS; do echo "    $r  $(du -sk "$REMOTE_OUT/$r" 2>/dev/null | cut -f1) kB  $(kvf "$REMOTE_OUT/$r/summary.txt" nodes | sed 's/^/nodes /')"; done
    echo "  slurm jobs found: $(echo $JOBS | tr '\n' ' ')"; echo "  hosts with capture files: $(echo "$HOSTL" | tr ',' '\n' | grep -c .)"
    echo "  meta: site.env, kit manifest check, git, binaries, modules, versions, sacct/scontrol per job, sinfo, topology, digit_files.txt"
    echo "  excluded: scratch/, *.part*, files over 4 GiB; digit_files= paths of the summaries are recorded"
    exit 0
fi
mkdir -p "$OUTD" || { echo "collect.sh: cannot create $OUTD" >&2; exit 2; }
STG=$(mktemp -d "$OUTD/.collect_stage.XXXXXX") || exit 2; trap 'rm -rf "$STG"' EXIT
M=$STG/meta; mkdir -p "$M"
sec() { grep -a -v -i -E '^[^=]*(KEY|TOKEN|PASS|SECRET|CRED|AUTH|COOKIE)[^=]*='; }
t() { rl_tmo "${TMOS:-60}" "$@" 2>&1; }
# ---- kit identity, site
[ -f "$SITE_ENV" ] && sec < "$SITE_ENV" > "$M/site.env"
{ echo "kit dir: $R"; echo "date: $(date '+%F %T %Z')"; echo "host: $(hostname)"; echo "user: $(rl_user)"
  if [ -f "$R/remote/MANIFEST.sha256" ]; then
      ( cd "$R" && sha256sum -c remote/MANIFEST.sha256 2>&1 ) > "$M/manifest_check.txt"; echo "manifest check: $(grep -c ': OK$' "$M/manifest_check.txt") OK, $(grep -c -v ': OK$' "$M/manifest_check.txt") not OK (see manifest_check.txt)"
  else echo "manifest check: no remote/MANIFEST.sha256"; fi
  if [ -e "$R/.git" ] && rl_have git; then echo "git commit: $(git -C "$R" rev-parse HEAD 2>&1)"; git -C "$R" status --short 2>&1 | head -20; else echo "git: no .git in the kit copy"; fi
  [ -f "$R/remote/VERSION" ] && cat "$R/remote/VERSION"; } > "$M/kit.txt" 2>&1
# ---- binaries, modules, versions
{ for p in 1 2; do b=$(rl_bin $p); [ -n "$b" ] && [ -f "$b" ] && { sha256sum "$b"; ls -l "$b"; echo "-- ldd p$p"; t ldd "$b"; }; done
  for f in "$R"/ecalc/mnrun.sh "$R"/ecalc/mn_model.py "$R"/tools/plan_values.py; do [ -f "$f" ] && sha256sum "$f"; done; } > "$M/binaries.txt" 2>&1
{ rl_module_init; set +u; echo "-- module list"; module list; echo "-- site modules: p1 '$(rl_mods 1)' p2 '$(rl_mods 2)' unload '$(rl_unload)'"; } > "$M/modules.txt" 2>&1
{ echo "-- uname"; uname -a; echo "-- os-release"; head -4 /etc/os-release 2>/dev/null
  for c in "gcc --version" "cc --version" "hipcc --version" "amd-smi version" "fi_info --version" "srun --version" "python3 --version" "make --version"; do set -- $c; rl_have "$1" && { echo "-- $c"; t $c | head -8; }; done
  echo "-- rocm"; ls -d /opt/rocm* 2>/dev/null; cat /opt/rocm/.info/version 2>/dev/null
  echo "-- shmem / libfabric / cray packages"; ls /opt/cray/pe/sma /opt/cray/libfabric 2>/dev/null | head -20
  rl_have rpm && rpm -qa 2>/dev/null | grep -i -E 'libfabric|cray-shmem|openshmemx|dsmml|rocm-core|rocm-hip|cxi|slingshot' | sort | head -40
  echo "-- env of the kit"; env | sort | grep -a -E '^(REMOTE_|MNRUN_|ECALC_|XSHMEM_|FI_|SHMEM_|COMM_|SLURM_JOB|TGT_)' | sec; } > "$M/versions.txt" 2>&1
# ---- Slurm
mkdir -p "$M/slurm"
for j in $JOBS; do
    if rl_have sacct; then TMOS=120 t sacct -j "$j" --format=ALL -P > "$M/slurm/sacct_$j.txt"; fi
    if rl_have scontrol; then t scontrol show job -dd "$j" > "$M/slurm/scontrol_job_$j.txt"; grep -q -i 'invalid job id' "$M/slurm/scontrol_job_$j.txt" 2>/dev/null && rm -f "$M/slurm/scontrol_job_$j.txt"; fi
done
if [ -n "$HOSTL" ]; then
    rl_have scontrol && { TMOS=120 t scontrol show node "$HOSTL" -o > "$M/slurm/scontrol_nodes.txt"; }
    rl_have sinfo && { t sinfo -N -n "$HOSTL" -o '%N %P %T %c %m %G %f %E' > "$M/slurm/sinfo_nodes.txt"; }
fi
rl_have sinfo && t sinfo -o '%P %a %l %D %T %N' | head -50 > "$M/slurm/sinfo.txt"
rl_have scontrol && { TMOS=120 t scontrol show topology | head -c 4000000 > "$M/slurm/topology.txt"; }
[ -n "$(ls -A "$M/slurm" 2>/dev/null)" ] || echo "no Slurm tool answered (or no job id found)" > "$M/slurm/NONE.txt"
# ---- digit files (not bundled): path, size, printed hash
{ echo "# digit files are NOT in the bundle; path, bytes now, hash the run printed"
  for r in $RUNS; do f=$REMOTE_OUT/$r/summary.txt; dfd=$(kvf "$f" digit_files); [ -n "$dfd" ] || continue
      echo "$r digits=$(kvf "$f" digits) dir=$dfd size_bytes=$(du -cb "$dfd" 2>/dev/null | tail -1 | cut -f1) digit_hash=$(kvf "$f" digit_hash) last_digits=$(kvf "$f" last_digits)"; done; } > "$M/digit_files.txt"
# ---- file list: every run dir in full; skip digit-sized files
( cd "$REMOTE_OUT" && for r in $RUNS; do find "$r" \( -type f -o -type l \) \( -name '*.part[0-9]*' -o -size +4G \) -print; done ) > "$M/skipped_files.txt"
( cd "$REMOTE_OUT" && for r in $RUNS; do find "$r" -type f; done | grep -F -v -x -f "$M/skipped_files.txt" ) > "$STG/files.lst"
# ---- README, MANIFEST
cat > "$STG/README.txt" <<EOT
ecalc remote session bundle $NAME (made $(date '+%F %T %Z') by remote/run/collect.sh). PRIVATE: it holds host names and paths; do not publish.
runs/<run dir>/        every run dir of the session in full: summary.txt (the pasted block), log.txt, raw/*.log(.gz) (the whole computation log of the run: ecalc's output of
                       every rank, MN_WAIT_STATS / ECALC_INIT_TL / NEWTON_MN_RECIP_TS lines, memory tables), raw/fit_* (planner), capture/ (launch line, env, modules,
                       scontrol job, hosts, binary sha256, mnrun's computed values), snap/before|after/<host>.txt (node state before and after: meminfo, numastat, amd-smi,
                       every nonzero Slingshot counter), snap/nic_summary.csv (after minus before per host and NIC), monitor/<host>.csv (timeline every few seconds)
site.env               the session's site settings (secrets dropped)
meta/kit.txt           kit copy: manifest check result, git commit if any; meta/manifest_check.txt has the per-file result
meta/binaries.txt      sha256, size and ldd of the ecalc binaries; hashes of mnrun.sh, mn_model.py, plan_values.py
meta/modules.txt, versions.txt   module list, compiler / ROCm / libfabric / SHMEM / Slurm / OS versions
meta/slurm/            sacct -j <job> --format=ALL -P (all steps), scontrol show job -dd (while known), scontrol show node for the nodes used, sinfo, topology
meta/digit_files.txt   where the digit files are (not bundled), their size and the hash the run printed
meta/skipped_files.txt files left out for size / being digit parts
MANIFEST.txt           bytes and sha256 of every file in this bundle (except itself); check: cd <this dir> && sha256sum -c <(tail -n +2 MANIFEST.txt | awk -F'\t' '{print \$2"  "\$3}')
Runs: $(echo $RUNS | wc -w); Slurm jobs: $(echo $JOBS | tr '\n' ' ')
EOT
{ ( cd "$REMOTE_OUT" && xargs -d '\n' -r sha256sum < "$STG/files.lst" 2>/dev/null | while read -r h f; do printf '%s\t%s\t%s\n' "$(stat -c %s "$f")" "$h" "runs/$f"; done )
  ( cd "$STG" && find meta README.txt -type f | sort | xargs -r sha256sum | while read -r h f; do printf '%s\t%s\t%s\n' "$(stat -c %s "$f")" "$h" "$f"; done ); } > "$STG/MANIFEST.txt"
printf 'bytes\tsha256\tpath\n' | cat - "$STG/MANIFEST.txt" > "$STG/MANIFEST.tmp" && mv "$STG/MANIFEST.tmp" "$STG/MANIFEST.txt"
# ---- tar, hash
ln -s "$REMOTE_OUT" "$STG/runs"; { echo MANIFEST.txt; echo README.txt; (cd "$STG" && find meta -type f) ; sed 's,^,runs/,' "$STG/files.lst"; } > "$STG/tar.lst"
tar -czf "$BUNDLE.tmp" --transform "s,^,$NAME/," -C "$STG" -T "$STG/tar.lst" 2>"$STG/tar.err" || true
[ -s "$BUNDLE.tmp" ] || { echo "collect.sh: tar failed: $(head -3 "$STG/tar.err")" >&2; rm -f "$BUNDLE.tmp"; exit 1; }
mv "$BUNDLE.tmp" "$BUNDLE"
SHA=$(cd "$OUTD" && sha256sum "$NAME.tar.gz" | tee "$NAME.tar.gz.sha256" | cut -d' ' -f1); SIZE=$(stat -c %s "$BUNDLE")
NF=$(tar -tzf "$BUNDLE" 2>/dev/null | grep -c -v '/$'); NM=$(($(wc -l < "$STG/MANIFEST.txt") - 1))
echo "bundle=$BUNDLE size_bytes=$SIZE ($(awk -v s="$SIZE" 'BEGIN { printf "%.1f MB", s / 1e6 }')) sha256=$SHA files=$NF manifest_entries=$NM runs=$(echo $RUNS | wc -w) jobs=$(echo $JOBS | wc -w)" | tee "$OUTD/collect_latest.txt"
[ -s "$M/skipped_files.txt" ] && echo "left out: $(wc -l < "$M/skipped_files.txt") digit-sized file(s), see meta/skipped_files.txt"
if [ "$PASTE" = 1 ]; then
    echo "=== REMOTE collect DIGEST (one line per run) ==="
    echo "runs: n | nodes digits path verdict | total_s phases(init/bs/dm/dc) | apu_peak/plan GB | nic tx GB/node | verify last20 hash | settings"
    nl=0
    for r in $RUNS; do f=$REMOTE_OUT/$r/summary.txt; [ -f "$f" ] || continue; case $r in s5_*) ;; *) continue;; esac
        grep -aq 'verdict=SKIP' "$f" && continue; [ -n "$(kvf "$f" nodes)" ] || continue; nl=$((nl + 1)); [ $nl -le 48 ] || continue
        v=$(grep -a -m1 -o 'verdict=[A-Z]*' "$f" | cut -d= -f2)
        ph=$(grep -a -m1 '^phases_s:' "$f" | sed -E 's/.*init=([^ ]*) bs=([^ ]*) recip=[^ ]* dm=([^ ]*) dc=([^ ]*).*/\1\/\2\/\3\/\4/')
        pk=$(kvf "$f" device_peak_apu_gb | awk '{ print $1 }'); pl=$(kvf "$f" device_plan_apu_gb | awk '{ print $1 }')
        nic=$(kvf "$f" nic_gb_per_node | awk '{ print ($1 == "tx") ? $2 : "-" }')
        echo "$(printf '%s | %s %s p%s %s | %s %s | %s/%s | %s | %s %s %s | %s' "$nl" "$(kvf "$f" nodes)n" "$(kvf "$f" digits)" "$(kvf "$f" path)" "$v" "$(kvf "$f" total_s)" "${ph:-NA}" "${pk:-NA}" "${pl:-NA}" "${nic:--}" "$(kvf "$f" verify)" "$(kvf "$f" last_digits | tr -d ' ' | cut -c1-20)" "$(kvf "$f" digit_hash | awk '$1 ~ /^[0-9a-f]+$/ { print $1; next } { print "-" }')" "$(kvf "$f" settings | cut -c1-110)")" | rl_tilde
    done
    [ $nl -gt 0 ] || echo "(no s5_run summary under REMOTE_OUT)"
    [ $nl -le 48 ] || echo "... $((nl - 48)) more run(s) in the bundle"
    echo "bundle: $NAME.tar.gz  size $(awk -v s="$SIZE" 'BEGIN { printf "%.1f MB", s / 1e6 }')  sha256 $SHA  ($NF files; private: the user transfers it)"
    echo "=== END collect DIGEST ==="
fi
exit 0
