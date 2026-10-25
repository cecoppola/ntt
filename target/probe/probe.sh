#!/bin/bash
# target/probe/probe.sh - step 1 of the target session: FAST, READ-ONLY collector of every system fact needed to work on the
# machine and to generate config/target.conf (probe/gen_conf.py).  Contracts: target/FORMATS.md (layout, summary.txt, lib/common.sh).
#
#   target/probe/probe.sh [--only login|node] [--jobid J] [--out DIR] [--dry-run]
#                         [--want-rocm 7.14.0] [--want-sma 12.1] [--want-fi 2.3.1] [--no-shmem]
#
#   (no --only)   login part here; if a job is known (--jobid J, else $SLURM_JOB_ID) also the node part.
#   --only login  login part: OS, modules, compilers, libraries, Slurm and queue rules, file systems, limits.  Budget 2 min.
#   --only node   node part inside the one-node allocation J: srun runs this script on the node (--on-node, internal) for CPU,
#                 memory, APUs, ROCm, NICs, node location, libfabric, userfaultfd; then the SHMEM test is built and run with
#                 1, 2 and 4 PEs (plus NIC-count variants).  Budget 5 min.  The repository must be on a file system the node sees.
#   --out DIR     output dir of the part (default target/results/probe/<host>-<YYYYmmdd-HHMM>); with both parts DIR/login, DIR/node.
#   --build-only  only build the SHMEM test on this machine (modules picked from the facts); no srun.
#   --dry-run     print the commands (with their time limits) and stop; nothing is run or written.
#   PROBE_SRUN_EXTRA="--gpus-per-node=4"   extra srun options (if the site hides the APUs from a step).
#   Every call is wrapped in `timeout 10` (exceptions: the SHMEM build 60 s and runs 40 s).  A missing command, a timeout or a
#   skipped call (budget used up) never stops the probe: the fact is recorded as `missing` (or `none` = ran, nothing found).
# Output dir: facts.tsv (key TAB value TAB source), raw/<name>.txt (full output of each call), raw/_commands.tsv (name, command,
#   rc, seconds), summary.txt, DONE.  The scontrol dump of ALL nodes is raw/scontrol_nodes.txt (login part; ladder/locality.py).
# Exit 0 = the probe finished (facts may be missing), 2 = usage.
#
# ---- fact keys (stable; value text is as the tool prints it, trimmed, newlines as " / ") ------------------------------------------
# sys.*     os kernel arch hostname hostname_full hostname_scheme (digits -> N) uptime user_id
# cpu.*     model sockets cores_per_socket threads_per_core total allowed numa_nodes numa_cpus
# mem.*     total_kb available_kb free_kb hugepagesize_kb hugepages_total hugepages_free thp_enabled thp_defrag thp_shmem
#           hugetlb_2m_nr hugetlb_2m_free hugetlb_2m_per_numa numa_total_kb compaction_proactiveness shm_mount shm_size tmp_fstype
# gpu.*     count names vram_bytes gtt_bytes partition pci_numa_map xgmi_link_lines driver_version fw_lines smi_version rocm_smi
#           amd_smi (present|missing) kfd
# rocm.*    dirs version_file hipcc_version hip_clang_version hip_runtime_libs
# cc.*      cce (cc --version) craycc hipcc amdclang gcc make cmake python3
# mod.*     system loaded loaded_rocm avail_rocm avail_shmem avail_dsmml avail_libfabric avail_mpich avail_prgenv avail_craype_accel
#           avail_cce avail_gmp pick_rocm pick_sma pick_dsmml pick_modules spider_rocm spider_shmem
# lib.*     fi_path fi_version fi_providers fi_cxi_domain_count fi_cxi_domain_names fi_monitor.<name> libfabric_dirs libcxi cxi_stat_path
#           sma_prefix sma_versions sma_libs sma_libfabric dsmml_prefix gmp_h gmp_lib xpmem_dev xpmem_module knem_dev ptrace_scope
# net.*     cxi_count cxi_numa_map hsn_state hsn_speed_mbps ip_br lspci_cassini lspci_cassini_count cxi_stat_lines nics_per_apu
#           dragonfly (group/switch lines of cxi_stat or sysfs)
# loc.*     xname xname_parts nodeaddr nodehostname node_features node_gres scontrol_nodes_count
# slurm.*   version default_partition sinfo_summary partitions part.<name> cfg.<key> qos assoc reservations queue_load sshare
#           topology_switches topology_conf own_jobs
# fs.*      mounts mount.<mountpoint with / as _> dir.<VAR> lustre_df lustre_stripe.<n> shared_types
# lim.*     memlock nofile stack ulimit_a
# env.*     matching (names only) value.<NAME> for FI_ SHMEM XT_ CXI HSA HIP ROCR SLURM_JOB CRAY MPICH PMI vars
# uffd.*    sysctl syscall kernel_ge_6_1 madv_collapse
# shmem.*   build rc.<case> probe.<case> error.<case>   (cases: onepe twope fourpe nics2_1pe nics4_1pe nics8_1pe nics2_4pe)
# probe.*   part host start commit elapsed_s
# ----------------------------------------------------------------------------------------------------------------------------------

PROBE_SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
HERE=$(dirname "$PROBE_SELF")
. "$HERE/../lib/common.sh" || exit 2
tgt_init

BUILD_ONLY=0; ONLY=; JOBID=${SLURM_JOB_ID:-}; OUT_ARG=; DRY=0; ONNODE=0; DO_SHMEM=1
W_ROCM=7.14.0; W_SMA=12.1; W_FI=2.3.1
while [ $# -gt 0 ]; do case $1 in
  --only) ONLY=$2; shift;; --jobid) JOBID=$2; shift;; --out) OUT_ARG=$2; shift;; --dry-run) DRY=1;; --build-only) BUILD_ONLY=1;; --on-node) ONNODE=1;;
  --want-rocm) W_ROCM=$2; shift;; --want-sma) W_SMA=$2; shift;; --want-fi) W_FI=$2; shift;; --no-shmem) DO_SHMEM=0;;
  -h|--help) sed -n 2,/^# Exit/p "$0" | sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "probe.sh: unknown option $1" >&2; exit 2;; esac; shift; done
case "$ONLY" in ""|login|node) ;; *) echo "probe.sh: --only login|node" >&2; exit 2;; esac

UN=$(id -un 2>/dev/null); HN=$(hostname -s 2>/dev/null); TS=$(date +%Y%m%d-%H%M)
T0=$SECONDS; DEADLINE=110
EXTRA=${PROBE_SRUN_EXTRA:-}

# ---- primitives -------------------------------------------------------------------------------------------------------------------
# cap NAME CMD...: run CMD (<= ${T:-10} s, no stdin) into raw/NAME.txt; records rc and seconds in raw/_commands.tsv
cap() {
    local n=$1; shift
    if [ "$DRY" = 1 ]; then echo "[dry-run] ${T:-10}s  $n: $*" | cut -c1-200; return 0; fi
    local f=$RAW/$n.txt s=$SECONDS rc
    if [ $((SECONDS - T0)) -ge "$DEADLINE" ]; then
        echo "skipped_deadline" > "$f"; printf '%s\t%s\tskipped\t0\n' "$n" "$(echo "$*" | tr '\n\t' '  ')" >> "$RAW/_commands.tsv"; return 99
    fi
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing: $1" > "$f"; printf '%s\t%s\tmissing\t0\n' "$n" "$(echo "$*" | tr '\n\t' '  ')" >> "$RAW/_commands.tsv"; return 127
    fi
    timeout -k 2 "${T:-10}" "$@" > "$f" 2>&1 < /dev/null; rc=$?
    printf '%s\t%s\t%s\t%s\n' "$n" "$(echo "$*" | tr '\n\t' '  ')" "$rc" $((SECONDS - s)) >> "$RAW/_commands.tsv"
    return $rc
}
# capsh NAME NEEDS SCRIPT: like cap for a shell snippet; NEEDS is the binary it depends on ("-" = none)
capsh() {
    local n=$1 need=$2 sc=$3
    [ "$DRY" = 1 ] && { echo "[dry-run] ${T:-10}s  $n: ${sc%%$'\n'*}" | cut -c1-200; return 0; }
    if [ "$need" != - ] && [ "$DRY" != 1 ] && ! command -v "$need" >/dev/null 2>&1; then
        echo "missing: $need" > "$RAW/$n.txt"; printf '%s\t%s\tmissing\t0\n' "$n" "$(echo "$sc" | tr '\n\t' '  ')" >> "$RAW/_commands.tsv"; return 127
    fi
    cap "$n" bash -c "$sc"
}
# modrun ARGS...: the `module` command in a child shell (it is a shell function, timeout cannot run it directly)
modrun() { timeout -k 2 "${T:-10}" bash --noprofile --norc -c "${MODINIT:+. '$MODINIT' >/dev/null 2>&1;} type module >/dev/null 2>&1 || exit 127; module $*" 2>&1 < /dev/null; }
capmod() {   # NAME module-args...
    local n=$1; shift
    if [ "$DRY" = 1 ]; then echo "[dry-run] ${T:-10}s  $n: module $*"; return 0; fi
    local f=$RAW/$n.txt s=$SECONDS rc
    [ "$HAVE_MODULE" = 1 ] || { echo "missing: module" > "$f"; printf '%s\tmodule %s\tmissing\t0\n' "$n" "$*" >> "$RAW/_commands.tsv"; return 127; }
    if [ $((SECONDS - T0)) -ge "$DEADLINE" ]; then echo skipped_deadline > "$f"; printf '%s\tmodule %s\tskipped\t0\n' "$n" "$*" >> "$RAW/_commands.tsv"; return 99; fi
    modrun "$@" > "$f"; rc=$?
    printf '%s\tmodule %s\t%s\t%s\n' "$n" "$*" "$rc" $((SECONDS - s)) >> "$RAW/_commands.tsv"; return $rc
}
fact() {     # KEY VALUE [SOURCE]
    [ "$DRY" = 1 ] && return 0
    local v=$2; v=${v//$'\t'/ }; v=${v//$'\r'/}; v=${v//$'\n'/ / }
    v=${v#"${v%%[![:space:]]*}"}; v=${v%"${v##*[![:space:]]}"}
    [ -n "$v" ] || v=none
    printf '%s\t%.3000s\t%s\n' "$1" "$v" "${3:-}" >> "$FACTS"
}
rawstate() {  # NAME: ok | missing | timeout | skipped (from _commands.tsv)
    awk -F'\t' -v n="$1" '$1==n{r=$3} END{ if(r=="") print "missing"; else if(r=="missing"||r=="skipped") print r; else if(r==124||r==137) print "timeout"; else print "ok"}' "$RAW/_commands.tsv" 2>/dev/null
}
# fx KEY NAME PIPELINE: fact = PIPELINE applied to raw/NAME.txt (stdin).  missing / timeout when the call did not deliver.
fx() {
    [ "$DRY" = 1 ] && return 0
    local st v; st=$(rawstate "$2"); v=
    case $st in missing|skipped) fact "$1" missing "raw/$2.txt"; return;; esac
    v=$(bash -c "$3" < "$RAW/$2.txt" 2>/dev/null | head -c 3000)
    [ -z "$v" ] && [ "$st" = timeout ] && v=timeout
    fact "$1" "$v" "raw/$2.txt"
}
# fxm PREFIX NAME PIPELINE: PIPELINE prints "key TAB value" lines; each becomes fact PREFIX.key
fxm() {
    [ "$DRY" = 1 ] && return 0
    local st k v; st=$(rawstate "$2")
    case $st in missing|skipped) fact "$1" missing "raw/$2.txt"; return;; esac
    while IFS=$'\t' read -r k v; do [ -n "$k" ] && fact "$1.$k" "$v" "raw/$2.txt"; done < <(bash -c "$3" < "$RAW/$2.txt" 2>/dev/null | head -n 400)
}

init_modules() {
    HAVE_MODULE=0; MODINIT=
    if type module >/dev/null 2>&1; then HAVE_MODULE=1; fi
    local f
    for f in /etc/profile.d/modules.sh /usr/share/Modules/init/bash /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /etc/profile.d/lmod.sh; do
        [ -r "$f" ] && { MODINIT=$f; break; }
    done
    if [ "$HAVE_MODULE" = 0 ] && [ -n "$MODINIT" ]; then . "$MODINIT" >/dev/null 2>&1; type module >/dev/null 2>&1 && HAVE_MODULE=1; fi
    return 0
}

# ---- common (login and node): OS, limits, environment, uffd -----------------------------------------------------------------------------
part_common() {
    cap uname uname -a;                       fx sys.kernel uname 'awk "{print \$3}"' ; fx sys.arch uname 'awk "{print \$(NF-1)}"'
    capsh os_release - 'cat /etc/os-release'
    [ "$DRY" = 1 ] || fact sys.os "$(sed -n 's/^PRETTY_NAME=//p' "$RAW/os_release.txt" | tr -d '"')" raw/os_release.txt
    cap hostname hostname -f;                 fx sys.hostname_full hostname 'head -1'
    [ "$DRY" = 1 ] || { fact sys.hostname "$HN" hostname; fact sys.hostname_scheme "$(echo "$HN" | sed 's/[0-9][0-9]*/N/g')" hostname; fact sys.user_id "$(id -u 2>/dev/null)" id; }
    cap uptime uptime;                        fx sys.uptime uptime 'head -1'
    capsh ulimit - 'ulimit -a';               fx lim.memlock ulimit "sed -n 's/^max locked memory *([^)]*) *//p'"
    fx lim.nofile ulimit "sed -n 's/^open files *([^)]*) *//p'";  fx lim.stack ulimit "sed -n 's/^stack size *([^)]*) *//p'"
    fx lim.ulimit_a ulimit 'paste -sd";" | tr -s " "'
    capsh env - 'env | sort'
    fx env.matching env "grep -E '^(FI_|SHMEM|XT_|CXI|HSA|HIP|ROCR|SLURM|CRAY|MPICH|PMI|LMOD|ROCM)' | cut -d= -f1 | paste -sd' '"
    fxm env.value env "grep -E '^(FI_|SHMEM|XT_|CXI|HSA|HIP|ROCR|CRAY_|MPICH|ROCM|SLURM_JOB_(ID|PARTITION|NODELIST|NUM_NODES)|SLURM_CLUSTER)' | grep -viE 'pass|token|key=' | sed 's/=/\t/'"
    fact uffd.sysctl "$(cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null)" /proc/sys/vm/unprivileged_userfaultfd
    capsh uffd_syscall python3 'python3 -I -c "import ctypes,os
l=ctypes.CDLL(None,use_errno=True)
for nm,fl in ((\"plain\",0),(\"user_mode_only\",1)):
    fd=l.syscall(323, os.O_CLOEXEC|os.O_NONBLOCK|fl)
    print(\"%s:%s\"%(nm,\"works\" if fd>=0 else \"fails_errno=%d\"%ctypes.get_errno()),end=\" \")"'
    fx uffd.syscall uffd_syscall 'head -1'
    [ "$DRY" = 1 ] || fact uffd.kernel_ge_6_1 "$(uname -r | awk -F'[.-]' '{print ($1>6||($1==6&&$2>=1))?"yes":"no"}')" uname
    capsh madv python3 'python3 -I -c "import mmap
e=None
for k in range(3):
    try:
        m=mmap.mmap(-1,16<<20)
        for i in range(0,16<<20,4096): m[i]=1
        m.madvise(25); print(\"works\"); break
    except OSError as x: e=x
else: print(\"errno=%d\"%e.errno)"'
    fx uffd.madv_collapse madv 'head -1'
}

# ---- login part -------------------------------------------------------------------------------------------------------------------------
part_login() {
    part_common
    # compilers
    capsh cc_version cc 'cc --version';       fx cc.cce cc_version 'head -2 | paste -sd" "'
    cap craycc craycc --version;              fx cc.craycc craycc 'head -1'
    cap hipcc hipcc --version;                fx cc.hipcc hipcc 'paste -sd" " | cut -c1-300'
    cap amdclang amdclang --version;          fx cc.amdclang amdclang 'head -1'
    cap gcc gcc --version;                    fx cc.gcc gcc 'head -1'
    cap make make --version;                  fx cc.make make 'head -1'
    cap cmake cmake --version;                fx cc.cmake cmake 'head -1'
    cap python3 python3 --version;            fx cc.python3 python3 'head -1'
    # modules
    init_modules
    [ "$DRY" = 1 ] || fact mod.system "$([ "$HAVE_MODULE" = 1 ] || [ -n "$MODINIT" ] && { [ -n "${LMOD_VERSION:-}" ] && echo "lmod ${LMOD_VERSION}" || echo "tcl-or-other"; } || echo none)" env
    capmod module_avail -t avail
    capmod module_list -t list
    AV='sed "s/(default)//;s/(D)//;s/(L)//" | grep -E "^[A-Za-z0-9_.+-]+/[0-9]" | sort -u'
    fx mod.loaded module_list "grep -oE '[A-Za-z0-9_.+-]+/[0-9][A-Za-z0-9_.+-]*' | paste -sd' '"
    fx mod.loaded_rocm module_list "grep -oE '(rocm|amd)[A-Za-z0-9_.+-]*/[0-9][A-Za-z0-9_.+-]*' | paste -sd' '"
    fx mod.avail_rocm module_avail "$AV | grep -iE '^(rocm|amd)' | paste -sd' '"
    fx mod.avail_shmem module_avail "$AV | grep -iE '^(cray-openshmemx|cray-shmem|openshmem|rocshmem|cray-sma)' | paste -sd' '"
    fx mod.avail_dsmml module_avail "$AV | grep -iE '^cray-dsmml' | paste -sd' '"
    fx mod.avail_libfabric module_avail "$AV | grep -iE '^(libfabric|cray-libfabric|cray-xpmem|xpmem)' | paste -sd' '"
    fx mod.avail_mpich module_avail "$AV | grep -iE '^cray-mpich' | paste -sd' '"
    fx mod.avail_prgenv module_avail "$AV | grep -iE '^PrgEnv' | paste -sd' '"
    fx mod.avail_craype_accel module_avail "$AV | grep -iE '^craype-accel' | paste -sd' '"
    fx mod.avail_cce module_avail "$AV | grep -iE '^(cce|cray-cce)/' | paste -sd' '"
    fx mod.avail_gmp module_avail "$AV | grep -iE '^(gmp|cray-gmp)' | paste -sd' '"
    if [ "$DRY" != 1 ]; then
        local av r s d pm
        av=$(sed 's/(default)//;s/(D)//;s/(L)//' "$RAW/module_avail.txt" 2>/dev/null | grep -E '^[A-Za-z0-9_.+-]+/[0-9]' | sort -u)
        r=$(echo "$av" | grep -iE '^(rocm|amd)/' | grep -F "$W_ROCM" | grep -vE 'afar|therock|new' | head -1)
        s=$(echo "$av" | grep -iE '^(cray-openshmemx|cray-shmem|cray-sma)' | grep -F "$W_SMA" | head -1)
        d=$(echo "$av" | grep -iE '^cray-dsmml' | sort -V | tail -1)
        pm=; [ -n "$r" ] && [ -n "$s" ] && [ -n "$d" ] && pm="$d $s $r"
        fact mod.pick_rocm "${r:-none}" "match $W_ROCM"; fact mod.pick_sma "${s:-none}" "match $W_SMA"; fact mod.pick_dsmml "${d:-none}" "highest cray-dsmml"
        fact mod.pick_modules "${pm:-none}" "dsmml sma rocm"
    fi
    # libraries (login view; node part repeats the fi_info facts)
    part_libs
    capsh libs_dirs - 'ls -d /opt/cray/libfabric/* /opt/cray/pe/sma/* /opt/cray/pe/dsmml/* /opt/rocm* /opt/cray/pe/cce/* 2>/dev/null'
    fx lib.libfabric_dirs libs_dirs "grep libfabric | paste -sd' '"
    fx lib.sma_versions libs_dirs "grep '/sma/' | xargs -n1 basename | paste -sd' '"
    fx rocm.dirs libs_dirs "grep rocm | paste -sd' '"
    capsh gmp_find - 'ls /usr/include/gmp.h /usr/include/x86_64-linux-gnu/gmp.h "${CRAY_GMP_PREFIX:-/nonexistent}/include/gmp.h" /opt/cray/pe/gmp/*/include/gmp.h 2>/dev/null; ls /usr/lib64/libgmp.so* /usr/lib/x86_64-linux-gnu/libgmp.so* 2>/dev/null | head -3'
    fx lib.gmp_h gmp_find "grep 'gmp.h' | head -1";  fx lib.gmp_lib gmp_find "grep 'libgmp' | head -1"
    # Slurm and queue rules
    part_slurm
    # file systems
    part_fs
    # topology / switch info (login part: Slurm view)
    capsh topo_show scontrol 'scontrol show topology'
    fx slurm.topology_switches topo_show 'grep -c SwitchName'
    capsh topo_conf - 'head -c 20000 /etc/slurm/topology.conf'
    fx slurm.topology_conf topo_conf 'head -5 | paste -sd";" | cut -c1-400'
    # module spider (slow on big trees; last)
    capmod module_spider_rocm -t spider rocm;                fx mod.spider_rocm module_spider_rocm "grep -E '^rocm' | paste -sd' ' | cut -c1-400"
    capmod module_spider_shmem -t spider cray-openshmemx;    fx mod.spider_shmem module_spider_shmem "grep -E '^cray' | paste -sd' ' | cut -c1-400"
}

part_libs() {   # library facts valid on login and node
    cap fi_info_v fi_info --version;          fx lib.fi_version fi_info_v "grep -i 'libfabric:' | head -1"
    capsh fi_path - 'p=$(command -v fi_info) && { echo "$p"; ls -l "$p"; }'; fx lib.fi_path fi_path 'head -1'
    cap fi_list fi_info -l;                   fx lib.fi_providers fi_list "grep -E '^[a-z_;:+]+:' | tr -d ':' | paste -sd' '"
    cap fi_cxi fi_info -p cxi;                fx lib.fi_cxi_domain_count fi_cxi 'grep -c "domain:"'
    fx lib.fi_cxi_domain_names fi_cxi "grep 'domain:' | awk '{print \$2}' | sort -u | paste -sd' '"
    capsh libcxi - 'ldconfig -p 2>/dev/null | grep -E "libcxi|libfabric|libsma\.so|libdsmml|libhsa|libamdhip"; rpm -q libcxi libcxi-utils 2>&1 | head -3'
    fx lib.libcxi libcxi "grep -E 'libcxi|libfabric|libsma[.]so|libdsmml' | paste -sd';' | cut -c1-600"
    capsh cxi_stat_path - 'command -v cxi_stat cxi_diag cxi_udp_gen 2>/dev/null'; fx lib.cxi_stat_path cxi_stat_path 'paste -sd" "'
    capsh sma_find - 'for d in ${CRAY_OPENSHMEMX_PREFIX:-} /opt/cray/pe/sma/*/ofi/sma; do [ -d "$d" ] && echo "$d"; done; for d in ${CRAY_DSMML_PREFIX:-} /opt/cray/pe/dsmml/*/dsmml; do [ -d "$d" ] && echo "DSMML $d"; done'
    fx lib.sma_prefix sma_find "grep -v '^DSMML ' | sort -V | tail -1"
    fx lib.dsmml_prefix sma_find "grep '^DSMML ' | sed 's/^DSMML //' | sort -V | tail -1"
    capsh sma_ldd ldd 'p=$(ls -d /opt/cray/pe/sma/*/ofi/sma 2>/dev/null | sort -V | tail -1); ldd "$p"/lib64/libsma.so 2>&1 | head -40'
    fx lib.sma_libs sma_ldd "grep -oE 'lib(xpmem|fabric|cxi|hsa-runtime64|amdhip64|pmi2?|ze_loader)[^ ]*' | sort -u | paste -sd' '"
    fx lib.sma_libfabric sma_ldd "awk '/libfabric/{print \$3}' | head -1"
}

part_slurm() {
    cap sinfo_v sinfo --version;              fx slurm.version sinfo_v 'head -1'
    cap sinfo_s sinfo -s;                     fx slurm.sinfo_summary sinfo_s 'paste -sd";" | tr -s " "'
    cap sinfo_states sinfo -h -o '%P|%t|%D|%f|%G'
    fx slurm.default_partition sinfo_states "grep '\*' | head -1 | cut -d'|' -f1 | tr -d '*'"
    fx slurm.partitions sinfo_states "cut -d'|' -f1 | tr -d '*' | sort -u | paste -sd' '"
    fxm slurm.nodes_by_state sinfo_states "awk -F'|' '{k=\$1; gsub(/\\*/,\"\",k); k=k\".\"\$2; n[k]+=\$3; f[k]=\$4\" gres=\"\$5} END{for(k in n) print k\"\t\"n[k]\" nodes (last features=\"f[k]\")\"}' | sort"
    cap scontrol_part scontrol show partition -o
    fxm slurm.part scontrol_part "sed -E 's/^PartitionName=([^ ]+) (.*)/\1\t\2/' | awk -F'\t' '{n=split(\$2,a,\" \"); o=\"\"; for(i=1;i<=n;i++) if(a[i] ~ /^(MaxNodes|MaxTime|DefaultTime|QoS|OverSubscribe|State|TotalNodes|TotalCPUs|AllowAccounts|AllowGroups|AllowQos|Default|MinNodes|MaxCPUsPerNode|PriorityTier|ExclusiveUser|PreemptMode|TRES|DefMemPerNode|MaxMemPerNode|Nodes)=/) o=o\" \"a[i]; print \$1\"\t\"o}'"
    T=15 cap scontrol_nodes scontrol show node -o
    fx loc.scontrol_nodes_count scontrol_nodes 'grep -c "^NodeName="'
    cap scontrol_cfg scontrol show config
    fxm slurm.cfg scontrol_cfg "awk '/^(SelectType|SelectTypeParameters|PreemptMode|PreemptType|PriorityType|SchedulerParameters|SchedulerType|MaxJobCount|PrologFlags|Prolog|Epilog|PrologSlurmctld|EpilogSlurmctld|RebootProgram|ResumeProgram|TopologyPlugin|TopologyParam|SwitchType|TaskPlugin|ProctrackType|MpiDefault|DefMemPerNode|DefMemPerCPU|GresTypes|UnkillableStepTimeout|MinJobAge|MaxArraySize|MaxStepCount|SlurmctldTimeout|ReturnToService|ResumeTimeout|SuspendTime|JobSubmitPlugins|AccountingStorageEnforce|AccountingStorageTRES|SrunPortRange)[ \t]*=/{k=\$1; sub(/^[^=]*= */,\"\"); print k\"\t\"\$0}'"
    cap sacctmgr_qos sacctmgr -nP show qos format=Name,Priority,MaxWall,MaxTRESPU,MaxJobsPU,MaxSubmitPU,MaxTRES,GrpTRES,Flags,Preempt
    fx slurm.qos sacctmgr_qos 'paste -sd";" | cut -c1-1500'
    cap sacctmgr_assoc sacctmgr -nP show assoc where user="$UN" format=Account,Partition,QOS,MaxJobs,MaxSubmit,MaxTRES,MaxTRESPU,GrpTRES,MaxWall,GrpJobs
    fx slurm.assoc sacctmgr_assoc 'sort -u | head -12 | paste -sd";" | cut -c1-1500'
    cap scontrol_resv scontrol show reservation -o
    fx slurm.reservations scontrol_resv 'cut -c1-300 | head -8 | paste -sd";"'
    capsh squeue_states squeue 'squeue -h -o %T | sort | uniq -c'
    fx slurm.queue_load squeue_states "awk '{printf \"%s=%s \", \$2, \$1}'"
    capsh squeue_own squeue "squeue -h -u '$UN' -o '%i %P %T %D %l'"
    fx slurm.own_jobs squeue_own 'wc -l'
    cap sshare sshare -U -u "$UN";            fx slurm.sshare sshare 'head -6 | paste -sd";" | tr -s " "'
}

part_fs() {
    cap df_hT df -hT
    cap df_PT df -PT -x tmpfs -x devtmpfs -x squashfs -x overlay -x efivarfs
    fxm fs.mount df_PT "awk 'NR>1 && \$3>1e8 {m=\$7; gsub(/\//,\"_\",m); sub(/^_/,\"\",m); if(m==\"\") m=\"root\"; printf \"%s\ttype=%s mount=%s size_kB=%s avail_kB=%s\n\", m, \$2, \$7, \$3, \$5}'"
    fx fs.shared_types df_PT "awk 'NR>1{print \$2}' | sort -u | grep -E 'lustre|gpfs|nfs|beegfs|daos|wekafs|panfs' | paste -sd' '"
    capsh fs_dirs df 'for v in HOME SCRATCH WORK PROJECT PROJWORK LUSTRE_HOME; do d=${!v:-}; [ -n "$d" ] && [ -d "$d" ] && { printf "%s\t" "$v"; echo "$d"; df -PT "$d" | tail -1; }; done'
    fxm fs.dir fs_dirs "paste - - | awk -F'\t' '{print \$1\"\t\"\$2\" \"\$3}' | head -20"
    capsh findmnt_shared findmnt 'findmnt -rn -o TARGET,FSTYPE,SOURCE | grep -E "lustre|gpfs|nfs|beegfs|daos|wekafs|panfs"'
    cap lustre_df lfs df -h
    fx fs.lustre_df lustre_df 'head -40 | grep -E "summary|filesystem_summary|^[a-z0-9_-]+-MDT0000|OST0000" | paste -sd";" | cut -c1-600'
    capsh lustre_stripe lfs 'for d in $(df -PT 2>/dev/null | awk "\$2==\"lustre\"{print \$7}" | head -4) ${SCRATCH:-} ${HOME:-}; do [ -d "$d" ] || continue; echo "== $d"; lfs getstripe -d "$d" 2>&1 | head -8; done'
    fx fs.lustre_stripe lustre_stripe 'paste -sd" " | tr -s " \t" " " | cut -c1-800'
}

# ---- node part (runs ON the node: probe.sh --on-node) -----------------------------------------------------------------------------------
part_node_facts() {
    part_common
    cap lscpu lscpu
    fx cpu.model lscpu "sed -n 's/^Model name: *//p' | head -1"
    fx cpu.sockets lscpu "sed -n 's/^Socket(s): *//p'";  fx cpu.cores_per_socket lscpu "sed -n 's/^Core(s) per socket: *//p'"
    fx cpu.threads_per_core lscpu "sed -n 's/^Thread(s) per core: *//p'";  fx cpu.total lscpu "sed -n 's/^CPU(s): *//p' | head -1"
    fx cpu.numa_nodes lscpu "sed -n 's/^NUMA node(s): *//p'"
    capsh nproc nproc 'nproc --all; nproc';   fx cpu.allowed nproc 'tail -1'
    cap numactl numactl -H;                   fx cpu.numa_cpus numactl "grep -E '^node [0-9]+ (cpus|size)' | paste -sd';' | cut -c1-1200"
    cap free free -h;                         cap meminfo cat /proc/meminfo
    fxm mem meminfo "awk '/^(MemTotal|MemAvailable|MemFree|Hugepagesize|HugePages_Total|HugePages_Free):/{k=\$1; sub(/:/,\"\",k); print k\"\t\"\$2\" \"\$3}' | sed -e 's/^MemTotal/total_kb/;s/^MemAvailable/available_kb/;s/^MemFree/free_kb/;s/^Hugepagesize/hugepagesize_kb/;s/^HugePages_Total/hugepages_total/;s/^HugePages_Free/hugepages_free/' | sed 's/ kB//'"
    capsh mm_sys - 'T=/sys/kernel/mm/transparent_hugepage; H=/sys/kernel/mm/hugepages/hugepages-2048kB
echo thp_enabled=$(cat $T/enabled 2>&1); echo thp_defrag=$(cat $T/defrag 2>&1); echo thp_shmem=$(cat $T/shmem_enabled 2>&1)
echo hugetlb_2m_nr=$(cat $H/nr_hugepages 2>&1); echo hugetlb_2m_free=$(cat $H/free_hugepages 2>&1)
echo compaction_proactiveness=$(cat /proc/sys/vm/compaction_proactiveness 2>&1)
for n in /sys/devices/system/node/node[0-9]*; do echo numa_total_kb.$(basename $n)=$(awk "/MemTotal/{print \$4}" $n/meminfo); echo hugetlb_2m_per_numa.$(basename $n)=$(cat $n/hugepages/hugepages-2048kB/free_hugepages 2>&1)/$(cat $n/hugepages/hugepages-2048kB/nr_hugepages 2>&1); done
echo shm_mount=$(findmnt -n -o TARGET,FSTYPE,OPTIONS /dev/shm 2>&1); echo shm_size=$(df -h /dev/shm 2>&1 | awk "NR==2{print \$2\" used \"\$3}"); echo tmp_fstype=$(findmnt -n -o FSTYPE -T /tmp 2>&1)'
    fxm mem mm_sys "sed 's/=/\t/'"
    # APUs
    cap rocm_smi_id rocm-smi --showid;        cap rocm_smi_product rocm-smi --showproductname
    cap rocm_smi_vram rocm-smi --showmeminfo vram;  cap rocm_smi_gtt rocm-smi --showmeminfo gtt
    cap rocm_smi_part rocm-smi --showcomputepartition
    cap rocm_smi_topo rocm-smi --showtopo;    cap rocm_smi_fw rocm-smi --showfwinfo
    cap rocm_smi_drv rocm-smi --showdriverversion;  cap rocm_smi_v rocm-smi --version
    cap amd_smi_list amd-smi list;            cap amd_smi_topo amd-smi topology;  cap amd_smi_fw amd-smi firmware;  cap amd_smi_v amd-smi version
    cap amd_smi_static amd-smi static --board
    fact gpu.rocm_smi "$(command -v rocm-smi >/dev/null 2>&1 && echo present || echo missing)" rocm-smi
    fact gpu.amd_smi "$(command -v amd-smi >/dev/null 2>&1 && echo present || echo missing)" amd-smi
    GPU_COUNT_DONE=0
    gpu_count() { [ "$DRY" = 1 ] && return 0; local c src=rocm-smi
        c=$(grep -oE '^GPU\[[0-9]+\]' "$RAW/rocm_smi_id.txt" 2>/dev/null | sort -u | wc -l)
        [ "$c" -gt 0 ] || { c=$(grep -c '^GPU: ' "$RAW/amd_smi_list.txt" 2>/dev/null); src=amd-smi; }
        [ "${c:-0}" -gt 0 ] || { c=$(wc -l < "$RAW/pci_gpu.txt" 2>/dev/null); src=pci; }
        fact gpu.count "${c:-0}" "$src"; }
    fx gpu.names rocm_smi_product "grep -i 'Card Series' | sed 's/.*: *//' | sort | uniq -c | paste -sd';' | tr -s ' '"
    fx gpu.vram_bytes rocm_smi_vram "grep 'Total Memory' | awk '{print \$NF}' | paste -sd' '"
    fx gpu.gtt_bytes rocm_smi_gtt "grep 'Total Memory' | awk '{print \$NF}' | paste -sd' '"
    fx gpu.partition rocm_smi_part "grep -iE 'partition' | sed 's/.*: *//' | sort | uniq -c | paste -sd';' | tr -s ' '"
    fx gpu.xgmi_link_lines rocm_smi_topo 'grep -ci xgmi'
    fx gpu.driver_version rocm_smi_drv "grep -i 'driver' | head -1 | sed 's/.*: *//'"
    fx gpu.fw_lines rocm_smi_fw "grep -iE 'GPU\[0\]' | paste -sd';' | tr -s ' \t' ' ' | cut -c1-800"
    fx gpu.smi_version rocm_smi_v 'head -2 | paste -sd" "'
    capsh pci_gpu - 'for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor 2>/dev/null)" = 0x1002 ] || continue; case "$(cat $d/class 2>/dev/null)" in 0x030000|0x038000|0x120000) echo "$(basename $d) numa=$(cat $d/numa_node 2>/dev/null)";; esac; done'
    fx gpu.pci_numa_map pci_gpu "awk '{print \$2}' | paste -sd' '"
    gpu_count
    fact gpu.kfd "$([ -d /sys/class/kfd/kfd ] && echo yes || echo no)" /sys/class/kfd
    # ROCm
    capsh rocm_ver - '[ -r /opt/rocm/.info/version ] && cat /opt/rocm/.info/version; ls /opt/rocm*/lib/libamdhip64.so.* 2>/dev/null | head -3'
    fx rocm.version_file rocm_ver "grep -E '^[0-9]+[.][0-9]' | head -1"; fx rocm.hip_runtime_libs rocm_ver "grep libamdhip | paste -sd' '"
    cap hipconfig hipconfig --version;        fx rocm.hipcc_version hipconfig 'head -1'
    capsh hipcc_v hipcc 'hipcc --version';    fx rocm.hip_clang_version hipcc_v "grep -i 'clang version' | head -1"
    # NICs
    capsh cxi_sys - 'for d in /sys/class/cxi/cxi*; do [ -e "$d" ] && echo "$(basename $d) numa=$(cat $d/device/numa_node 2>/dev/null) pci=$(basename $(readlink -f $d/device 2>/dev/null))"; done'
    fx net.cxi_numa_map cxi_sys "awk '{print \$1\":\"\$2}' | paste -sd' '"
    capsh hsn_sys - 'for d in /sys/class/net/hsn*; do [ -e "$d" ] && echo "$(basename $d) state=$(cat $d/operstate 2>/dev/null) speed=$(cat $d/speed 2>/dev/null) numa=$(cat $d/device/numa_node 2>/dev/null) mtu=$(cat $d/mtu 2>/dev/null)"; done'
    fx net.hsn_state hsn_sys "awk '{print \$1\"=\"\$2}' | sed 's/state=//' | paste -sd' '"
    fx net.hsn_speed_mbps hsn_sys "sed -n 's/.*speed=\([0-9-]*\).*/\1/p' | sort | uniq -c | paste -sd';' | tr -s ' '"
    [ "$DRY" = 1 ] || { local nc src=sys/class/cxi; nc=$(grep -c '^cxi' "$RAW/cxi_sys.txt" 2>/dev/null)
        [ "${nc:-0}" -gt 0 ] || { nc=$(grep -c '^hsn' "$RAW/hsn_sys.txt" 2>/dev/null); src=sys/class/net/hsn; }; fact net.cxi_count "${nc:-0}" "$src"; }
    capsh ip_br ip 'ip -br link; ip -br addr' ;  fx net.ip_br ip_br "grep -E '^(hsn|cxi)' | awk '{print \$1\" \"\$2}' | sort -u | paste -sd';'"
    capsh lspci lspci 'lspci -nn | grep -iE "cassini|slingshot|17db|ethernet.*cray"'
    fx net.lspci_cassini lspci 'cut -c1-100 | head -3 | paste -sd";"';  fx net.lspci_cassini_count lspci 'wc -l'
    capsh cxi_stat cxi_stat 'cxi_stat'
    fx net.cxi_stat_lines cxi_stat 'wc -l'
    fx net.dragonfly cxi_stat "grep -iE 'group|switch|nid|fabric|link|mac|speed' | head -24 | paste -sd';' | tr -s ' ' | cut -c1-800"
    [ "$DRY" = 1 ] || { local g n; g=$(awk -F'\t' '$1=="gpu.count"{print $2}' "$FACTS" | tail -1); n=$(awk -F'\t' '$1=="net.cxi_count"{print $2}' "$FACTS" | tail -1)
        case "$g$n" in *[!0-9]*|"") fact net.nics_per_apu none derived;; *) [ "$g" -gt 0 ] && fact net.nics_per_apu "$(awk -v n="$n" -v g="$g" 'BEGIN{printf "%.2f", n/g}')" "net.cxi_count / gpu.count" || fact net.nics_per_apu none derived;; esac; }
    # node location
    capsh xname_f - 'cat /etc/cray/xname 2>/dev/null; echo; cat /proc/cray_xt/nid 2>/dev/null'
    fx loc.xname xname_f 'head -1'
    fx loc.xname_parts xname_f "head -1 | sed -nE 's/^x([0-9]+)c([0-9]+)s([0-9]+)b([0-9]+)n([0-9]+)$/cab=\1 chassis=\2 slot=\3 blade=\4 node=\5/p'"
    capsh scontrol_me scontrol 'scontrol show node "$(hostname -s)" -o'
    fx loc.nodeaddr scontrol_me "grep -oE 'NodeAddr=[^ ]+' | head -1 | cut -d= -f2";  fx loc.nodehostname scontrol_me "grep -oE 'NodeHostName=[^ ]+' | head -1 | cut -d= -f2"
    fx loc.node_features scontrol_me "grep -oE 'AvailableFeatures=[^ ]+' | head -1 | cut -d= -f2"
    fx loc.node_gres scontrol_me "grep -oE ' Gres=[^ ]+' | head -1 | cut -d= -f2"
    # kernel / transport facts
    capsh xpmem_f - 'ls -l /dev/xpmem 2>&1; grep -c "^xpmem" /proc/modules; ls /dev/knem 2>&1; cat /proc/sys/kernel/yama/ptrace_scope 2>&1'
    fx lib.xpmem_dev xpmem_f "head -1 | grep -q '^c' && echo yes || echo no"; fx lib.xpmem_module xpmem_f 'sed -n 2p'
    fx lib.knem_dev xpmem_f "sed -n 3p | grep -q knem && echo yes || echo no"; fx lib.ptrace_scope xpmem_f 'tail -1'
    part_libs
    # libfabric MR-cache monitors (userfaultfd must be accepted; memhooks hangs ROCr, S46) - last, 5 calls
    local mon
    for mon in userfaultfd memhooks kdreg2 disabled; do
        capsh "fi_mon_$mon" fi_info "FI_MR_CACHE_MONITOR=$mon fi_info -p cxi 2>&1 | head -3"
        fx "lib.fi_monitor.$mon" "fi_mon_$mon" "awk '/provider:/{f=1} NR==1{l=\$0} END{if(f) print \"accepted\"; else print substr(l,1,100)}'"
    done
}

# ---- SHMEM test (driver side: modules, build, srun runs) --------------------------------------------------------------------------------
part_shmem() {
    local j=$1
    if [ "$DO_SHMEM" != 1 ]; then fact shmem.build skipped --no-shmem; return; fi
    init_modules
    local pm; pm=$(awk -F'\t' '$1=="mod.pick_modules"{print $2}' "$FACTS" 2>/dev/null | tail -1)
    if [ -z "$pm" ] || [ "$pm" = none ] || [ "$pm" = missing ]; then
        # login part not run in this dir: pick here
        if [ "$DRY" = 1 ]; then pm="<picked modules>"; else
            capmod module_avail -t avail
            local av r s d; av=$(sed 's/(default)//;s/(D)//;s/(L)//' "$RAW/module_avail.txt" 2>/dev/null | grep -E '^[A-Za-z0-9_.+-]+/[0-9]' | sort -u)
            r=$(echo "$av" | grep -iE '^(rocm|amd)/' | grep -F "$W_ROCM" | grep -vE 'afar|therock|new' | head -1)
            s=$(echo "$av" | grep -iE '^(cray-openshmemx|cray-shmem|cray-sma)' | grep -F "$W_SMA" | head -1)
            d=$(echo "$av" | grep -iE '^cray-dsmml' | sort -V | tail -1)
            pm=; [ -n "$r" ] && [ -n "$s" ] && [ -n "$d" ] && pm="$d $s $r"
            fact mod.pick_modules "${pm:-none}" "dsmml sma rocm"
        fi
    fi
    [ "$pm" != none ] && [ -n "$pm" ] || { fact shmem.build "no modules picked (rocm/sma/dsmml)" mod.pick_modules; return; }
    local cc=cc; [ "$DRY" = 1 ] || { command -v cc >/dev/null 2>&1 || cc=gcc; }
    local bin=$OUT/probe_shmem
    if [ "$DRY" = 1 ]; then
        echo "[dry-run] 60s  shmem_build: (module unload rocm family; module load $pm; $cc -O1 probe_shmem.c -lsma -lpmi -lpmi2 -ldsmml -o $bin)"
        local c; for c in onepe:1 twope:2 fourpe:4 nics2_1pe:1 nics4_1pe:1 nics8_1pe:1 nics2_4pe:4; do
            echo "[dry-run] 40s  shmem_${c%%:*}: srun --jobid=$j -N1 -n${c##*:} --overlap --mem=0 $EXTRA $bin"; done
        return 0
    fi
    (   # subshell: the module changes stay here
        if type module >/dev/null 2>&1; then
            for m in $(modrun -t list | grep -oE '(rocm|amd)[A-Za-z0-9_.+-]*/[0-9][A-Za-z0-9_.+-]*'); do module unload "$m" >/dev/null 2>&1; done
            module load $pm > "$RAW/module_load.txt" 2>&1; echo "rc=$?" >> "$RAW/module_load.txt"
        else echo "rc=127 (no module command)" > "$RAW/module_load.txt"; fi
        fact shmem.modules_loaded "$pm ($(tail -1 "$RAW/module_load.txt" 2>/dev/null))" raw/module_load.txt
        sma=${CRAY_OPENSHMEMX_PREFIX:-$(ls -d /opt/cray/pe/sma/*/ofi/sma 2>/dev/null | sort -V | tail -1)}
        dsm=${CRAY_DSMML_PREFIX:-$(ls -d /opt/cray/pe/dsmml/*/dsmml 2>/dev/null | sort -V | tail -1)}
        T=60 cap shmem_build "$cc" -O1 -o "$bin" "$HERE/probe_shmem.c" -I"$sma"/include -I"$dsm"/include -L"$sma"/lib64 -L"$dsm"/lib -L/opt/cray/pe/lib64 \
            -lsma -lpmi -lpmi2 -ldsmml -ldl -Wl,-rpath,"$sma"/lib64 -Wl,-rpath,"$dsm"/lib -Wl,-rpath,/opt/cray/pe/lib64
        if [ $? -ne 0 ] || [ ! -x "$bin" ]; then fact shmem.build "FAILED: $(head -c 200 "$RAW/shmem_build.txt" | tr '\n' ' ')" raw/shmem_build.txt; exit 0; fi
        fact shmem.build ok raw/shmem_build.txt
        export SHMEM_SYMMETRIC_SIZE=1073741824 XT_SYMMETRIC_HEAP_SIZE=1073741824
        sh_case() {   # LABEL NPES [VAR=val...]
            local lab=$1 n=$2; shift 2; local rc
            T=40 cap "shmem_$lab" env "$@" srun --jobid="$j" -N1 -n"$n" --ntasks-per-node="$n" --overlap --mem=0 $EXTRA "$bin"; rc=$?
            fact "shmem.rc.$lab" "$rc" "raw/shmem_$lab.txt"
            fx "shmem.probe.$lab" "shmem_$lab" "grep -a '^PROBE' | sed 's/^PROBE //' | sort -u | head -6 | paste -sd'|'"
            [ "$rc" = 0 ] || fx "shmem.error.$lab" "shmem_$lab" "grep -aiE 'error|abort|segm|invalid|only [0-9]+ nic|fatal' | head -2 | cut -c1-220 | paste -sd'|'"
        }
        [ "$BUILD_ONLY" = 1 ] && exit 0
        DEADLINE=290
        sh_case onepe 1
        sh_case twope 2
        sh_case fourpe 4
        sh_case nics2_1pe 1 SHMEM_OFI_NUM_NICS=2 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
        sh_case nics4_1pe 1 SHMEM_OFI_NUM_NICS=4 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
        sh_case nics8_1pe 1 SHMEM_OFI_NUM_NICS=8 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
        sh_case nics2_4pe 4 SHMEM_OFI_NUM_NICS=2 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
    )
}

# ---- driver -----------------------------------------------------------------------------------------------------------------------------
mkout() {   # NAME-OF-PART HOSTLABEL [DIR]: sets OUT RAW FACTS
    OUT=${3:-$TGT_RES/probe/$2-$TS}
    RAW=$OUT/raw
    [ "$DRY" = 1 ] && return 0
    tgt_sum_init "$OUT" "probe-$1"
    : > "$OUT/facts.tsv"; : > "$RAW/_commands.tsv"
    FACTS=$OUT/facts.tsv
}
finish() {   # PART
    [ "$DRY" = 1 ] && return 0
    local nm nt nk
    nm=$(awk -F'\t' '$2=="missing"' "$FACTS" | wc -l); nk=$(awk -F'\t' '$2=="missing"{print $1}' "$FACTS" | paste -sd' ')
    nt=$(awk -F'\t' '$3=="124"||$3=="137"||$3=="skipped"' "$RAW/_commands.tsv" | wc -l)
    tgt_kv part "$1"; tgt_kv facts "$(wc -l < "$FACTS")"; tgt_kv facts_missing "$nm"; tgt_kv missing_keys "$nk"
    tgt_kv calls "$(wc -l < "$RAW/_commands.tsv")"; tgt_kv calls_timeout_or_skipped "$nt"; tgt_kv elapsed_s $((SECONDS - T0))
    tgt_sum_end "$([ "$nt" = 0 ] && echo PASS || echo PARTIAL)" 0
    echo "probe $1: $OUT  facts=$(wc -l < "$FACTS") missing=$nm timeout_or_skipped=$nt wall=$((SECONDS - T0))s" >&2
}

run_login() {
    mkout login "$HN" "$1"; T0=$SECONDS; DEADLINE=110
    [ "$DRY" = 1 ] || { tgt_log "probe login part -> $OUT"; fact probe.part login; fact probe.host "$HN"; fact probe.commit "$TGT_SHA"; }
    part_login; finish login
}
run_node() {   # needs JOBID; the node facts run on the node via srun, the SHMEM test from here
    local j=$JOBID nh
    [ -n "$j" ] || { echo "probe.sh: the node part needs --jobid J (a running one-node allocation)" >&2; return 2; }
    if [ "$DRY" = 1 ]; then nh=NODE; else nh=$(timeout 20 srun --jobid="$j" -N1 -n1 --overlap --mem=0 $EXTRA hostname -s 2>/dev/null | head -1); [ -n "$nh" ] || nh=node; fi
    mkout node "$nh" "$1"; T0=$SECONDS; DEADLINE=290
    if [ "$DRY" = 1 ]; then
        echo "[dry-run] 240s  node_facts: srun --jobid=$j -N1 -n1 --overlap --mem=0 $EXTRA bash $PROBE_SELF --on-node --out <dir>   (runs the node-side calls below)"
        ONNODE=1; part_node_facts; part_shmem "$j"; return 0
    fi
    tgt_log "probe node part (job $j, node $nh) -> $OUT"
    fact probe.part node; fact probe.host "$nh"; fact probe.commit "$TGT_SHA"
    T=240 cap node_facts_run srun --jobid="$j" -N1 -n1 --overlap --mem=0 $EXTRA bash "$PROBE_SELF" --on-node --out "$OUT"
    [ "$(wc -l < "$FACTS")" -lt 8 ] && fact probe.error "node-side collection produced no facts: see raw/node_facts_run.txt" raw/node_facts_run.txt
    part_shmem "$j"; finish node
}

if [ "$ONNODE" = 1 ]; then      # internal: collect the node facts on this node into --out (facts.tsv appended)
    OUT=${OUT_ARG:?--on-node needs --out}; RAW=$OUT/raw; FACTS=$OUT/facts.tsv; mkdir -p "$RAW"; T0=$SECONDS; DEADLINE=230
    part_node_facts; exit 0
fi
if [ "$BUILD_ONLY" = 1 ]; then   # test aid: modules + SHMEM test build only, no srun (login node)
    mkout build "$HN" "$OUT_ARG"; T0=$SECONDS; DEADLINE=290; part_shmem 0; finish build; exit 0
fi
case "$ONLY" in
    login) run_login "$OUT_ARG";;
    node)  run_node "$OUT_ARG"; [ $? = 2 ] && exit 2;;
    *)     if [ -n "$OUT_ARG" ]; then run_login "$OUT_ARG/login"; else run_login ""; fi
           if [ -n "$JOBID" ]; then if [ -n "$OUT_ARG" ]; then run_node "$OUT_ARG/node"; else run_node ""; fi
           else echo "probe.sh: no --jobid and no \$SLURM_JOB_ID: node part not run (rerun with --only node --jobid J)" >&2; fi;;
esac
exit 0
