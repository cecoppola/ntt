# gres.sh - sourced (not run): MNRUN_GRES=auto|on|off, whether srun/sbatch/salloc get GPU options (--gpus*, --gpu-bind).
#   mn_gres_on [NODE] [PARTITION]   rc 0 = pass the GPU options, rc 1 = leave them out.  on / off decide at once; auto (default) asks Slurm
#                                   once and caches the answer in MNRUN_GRES_RESOLVED (exported, so child scripts do not ask again):
#                                   the node's `scontrol show node -o` (NODE), else every node of `sinfo -h -N -o %G` (PARTITION, else the default
#                                   partition) must list a gpu GRES.  Slurm unreachable or silent = unknown = rc 0 (the options stay, nothing changes).
#   mn_gres_strip "OPTIONS" [NODE] [PARTITION]   OPTIONS without the --gpus*=..., --gpu-bind=... words when the GPU options are off
# Why: a site whose compute nodes list no GRES (Gres=(null)) rejects every --gpus* option ("Invalid generic resource (gres)
# specification") although the GPUs are there; aac7 lists gpu:4, so auto keeps the options there.
mn_gres_on() {
    case "${MNRUN_GRES:-auto}" in on) return 0;; off) return 1;; auto) ;; *) echo "gres.sh: MNRUN_GRES=$MNRUN_GRES: auto, on or off" >&2; return 0;; esac
    case "${MNRUN_GRES_RESOLVED:-}" in on) return 0;; off) return 1;; esac
    local out=
    if [ -n "${1:-}" ] && command -v scontrol >/dev/null 2>&1; then
        out=$(timeout 30 scontrol show node "$1" -o 2>/dev/null | head -1)
        if [ -n "$out" ]; then case "$out" in *Gres=*gpu*) MNRUN_GRES_RESOLVED=on;; *) MNRUN_GRES_RESOLVED=off;; esac; fi
    fi
    if [ -z "${MNRUN_GRES_RESOLVED:-}" ] && command -v sinfo >/dev/null 2>&1; then
        local p=${2:-}   # no partition: the default one (sinfo marks it with *); other partitions may hold other GPU nodes
        [ -n "$p" ] || p=$(timeout 30 sinfo -h -o %P 2>/dev/null | sed -n 's/\*$//p' | head -1)
        out=$(timeout 30 sinfo -h -N ${p:+-p "$p"} -o %G 2>/dev/null)
        # on only if every node of the partition lists a gpu GRES
        if [ -n "$out" ]; then if echo "$out" | grep -qv gpu; then MNRUN_GRES_RESOLVED=off; else MNRUN_GRES_RESOLVED=on; fi; fi
    fi
    [ -n "${MNRUN_GRES_RESOLVED:-}" ] && export MNRUN_GRES_RESOLVED
    [ "${MNRUN_GRES_RESOLVED:-on}" = on ]
}
mn_gres_strip() {
    if mn_gres_on "${2:-}" "${3:-}"; then echo "$1"; return 0; fi
    local w r=; for w in $1; do case $w in --gpus=*|--gpus-per-*=*|--gpu-bind=*) continue;; esac; r="$r${r:+ }$w"; done; echo "$r"
}
