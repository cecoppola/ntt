#!/bin/bash
# tools/target_probe.sh (TASKS 28, N7E-T): READ-ONLY fact collector for a new machine (the production target or any aac-like cluster).
# Collects: module candidates (rocm, OpenSHMEM, dsmml, libfabric), ROCm version, APU count and memory, MemAvailable, cxi NICs per node and
# per NUMA node, libfabric providers and MR-cache monitors, userfaultfd, THP / hugepages / /dev/shm huge=, Slurm limits, and the SHMEM facts
# that decide the design (on-node path / shmem_ptr, SHMEM_LOCALITY, SHMEM_OFI_NUM_NICS and NIC policy acceptance, symmetric heap in device
# memory).  Then prints a PROPOSED target.conf (the template with every key it can determine filled) and the diff against the template.
# It writes only under --out (default ./target_probe_out); it changes nothing else: no module is left loaded in the caller's shell, no job is
# submitted, no file system is written outside --out (a tiny test binary is compiled there).  Output is scrubbed of user and host names.
#
#   tools/target_probe.sh [--out DIR] [--hold JOBID] [--template FILE] [--no-shmem] [--phase all|login|node|shmem]
#                         [--want-rocm 7.14.0] [--want-sma 12.1] [--want-fi 2.3.1] [--want-nics 8] [--tmo 90]
#   no --hold : everything runs on this machine (a login node: the node facts describe the login node and the SHMEM test is only built).
#   --hold J  : node facts and the SHMEM runs go to ONE node of the existing allocation J (srun --overlap --mem=0, one node, no -w);
#               login facts (modules, Slurm) still come from here.  Nothing is cancelled or submitted.
#   --want-*  : the versions the target is said to have (module candidates are matched by them; they are not module names).
#   PROBE_SRUN_EXTRA="--gpus-per-node=4" adds options to every srun step (if the site hides the APUs from a step without them).
#   Modules: PROBE_MODULES="m1 m2 ..." forces the module line; otherwise the probe picks the candidates matching --want-rocm / --want-sma and
#   the highest dsmml.  Stdout is bounded (about 150 lines); the full facts are in DIR/facts.txt, the proposal in DIR/target.conf.proposed.
# Docs: profiles/README.md "Target bring-up runbook".  Exit 0 = probe finished (the facts may say UNKNOWN/NONE), 2 = usage.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
OUT=./target_probe_out HOLD= TPL=$ROOT/profiles/target.conf.template DO_SHMEM=1 PHASE=all
W_ROCM=7.14.0 W_SMA=12.1 W_FI=2.3.1 W_NICS=8 TMO=90
while [ $# -gt 0 ]; do case $1 in
  --out) OUT=$2; shift;; --hold) HOLD=$2; shift;; --template) TPL=$2; shift;; --no-shmem) DO_SHMEM=0;; --phase) PHASE=$2; shift;;
  --want-rocm) W_ROCM=$2; shift;; --want-sma) W_SMA=$2; shift;; --want-fi) W_FI=$2; shift;; --want-nics) W_NICS=$2; shift;; --tmo) TMO=$2; shift;;
  -h|--help) sed -n 2,21p "$0"; exit 0;; *) echo "target_probe: unknown option $1" >&2; exit 2;; esac; shift; done
mkdir -p "$OUT" || exit 2; OUT=$(cd "$OUT" && pwd); F=$OUT/facts.txt
[ "$PHASE" = all ] && : > "$F"
UN=$(id -un); HN=$(hostname -s 2>/dev/null)
scrub() { sed -e "s#$HOME#~#g" -e "s#\b$UN\b#<user>#g" ${HN:+-e "s#\b$HN\b#<host>#g"}; }
kv() { printf '%s=%s\n' "$1" "$2" | scrub >> "$F"; }                      # one fact
sec() { printf '#### %s\n' "$1" >> "$F"; }
run() { timeout "${T:-30}" "$@" 2>&1 < /dev/null; }                      # bounded command
# node-side runner: on the hold's node (srun, one task) or here
nrun() { if [ -n "$HOLD" ]; then timeout "$TMO" srun --jobid="$HOLD" -N1 -n1 --overlap --mem=0 ${PROBE_SRUN_EXTRA:-} "$@" 2>&1 < /dev/null; else timeout "$TMO" "$@" 2>&1 < /dev/null; fi; }

# ---- modules ----------------------------------------------------------------------------------------------------------------------
init_modules() { type module >/dev/null 2>&1 && return 0
  for f in /etc/profile.d/modules.sh /usr/share/Modules/init/bash /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /etc/profile; do [ -r "$f" ] && . "$f" >/dev/null 2>&1; type module >/dev/null 2>&1 && return 0; done; return 1; }
modules_phase() {
  sec modules; init_modules || { kv MODULES_AVAILABLE no_module_command; return; }
  local av; av=$(module avail 2>&1 | tr -s ' \t' '\n\n' | sed 's/(default)//;s/(D)//;s/(L)//' | grep -E '/' | sort -u)
  echo "$av" > "$OUT/module_avail.txt"
  kv MOD_ROCM_CANDIDATES "$(echo "$av" | grep -iE '^(rocm|amd)/' | paste -sd' ')"
  kv MOD_SMA_CANDIDATES "$(echo "$av" | grep -iE '^(cray-openshmemx|cray-shmem|openshmem|rocshmem|cray-sma)' | paste -sd' ')"
  kv MOD_DSMML_CANDIDATES "$(echo "$av" | grep -iE '^cray-dsmml' | paste -sd' ')"
  kv MOD_FABRIC_CANDIDATES "$(echo "$av" | grep -iE '^(libfabric|cray-libfabric|cray-xpmem|xpmem)' | paste -sd' ')"
  kv MOD_GMP_CANDIDATES "$(echo "$av" | grep -iE '^(gmp|cray-gmp)' | paste -sd' ')"
  kv MOD_LOADED_NOW "$(module list 2>&1 | grep -oE '[A-Za-z0-9_.+-]+/[0-9][A-Za-z0-9_.+-]*' | paste -sd' ')"
  kv MOD_LOADED_ROCM "$(module list 2>&1 | grep -oE '(rocm|amd)[A-Za-z0-9_.+-]*/[0-9][A-Za-z0-9_.+-]*' | paste -sd' ')"
  local r s d
  if [ -n "${PROBE_MODULES:-}" ]; then PICK_MODULES=$PROBE_MODULES; kv PICK_MODULES "$PROBE_MODULES"; return; fi
  r=$(echo "$av" | grep -iE '^(rocm|amd)/' | grep -F "$W_ROCM" | grep -vE 'afar|therock|new' | head -1)
  [ -n "$r" ] || r=$(echo "$av" | grep -iE '^rocm/' | grep -F "$W_ROCM" | head -1)
  s=$(echo "$av" | grep -iE '^(cray-openshmemx|cray-shmem|cray-sma)' | grep -F "$W_SMA" | head -1)
  d=$(echo "$av" | grep -iE '^cray-dsmml' | sort -V | tail -1)
  kv PICK_ROCM "${r:-NONE_MATCHING_$W_ROCM}"; kv PICK_SMA "${s:-NONE_MATCHING_$W_SMA}"; kv PICK_DSMML "${d:-NONE_FOUND}"
  PICK_MODULES=; [ -n "$r" ] && [ -n "$s" ] && [ -n "$d" ] && PICK_MODULES=$(echo $d $s $r); kv PICK_MODULES "$PICK_MODULES"
}

# ---- machine (login or node) --------------------------------------------------------------------------------------------------------
node_phase_script() { cat <<'NODE'
kv() { printf '%s=%s\n' "$1" "$2"; }
kv NODE_CPUS "$(nproc --all 2>/dev/null)"; kv NODE_CPUS_ALLOWED "$(nproc 2>/dev/null)"
kv NODE_NUMA_NODES "$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)"
awk '/^(MemTotal|MemAvailable|MemFree|Hugepagesize|HugePages_Total|HugePages_Free|AnonHugePages|ShmemHugePages|Shmem):/{gsub(/:/,"",$1); printf "MEM_%s=%s %s\n",$1,$2,$3}' /proc/meminfo
for n in /sys/devices/system/node/node[0-9]*; do kv "NUMA_$(basename $n)_MemTotal_kB" "$(awk '/MemTotal/{print $4}' $n/meminfo)"; done
kv KERNEL "$(uname -r)"; kv OS "$(. /etc/os-release 2>/dev/null; echo $PRETTY_NAME)"
# huge pages
for f in enabled defrag shmem_enabled; do kv THP_$f "$(cat /sys/kernel/mm/transparent_hugepage/$f 2>/dev/null)"; done
kv SHM_MOUNT "$(findmnt -n -o TARGET,FSTYPE,OPTIONS /dev/shm 2>/dev/null)"
kv SHM_SIZE "$(df -h /dev/shm 2>/dev/null | awk 'NR==2{print $2" used "$3}')"
kv TMP_FSTYPE "$(findmnt -n -o FSTYPE -T /tmp 2>/dev/null)"
kv HUGETLB_2M_FREE "$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages 2>/dev/null)"; kv HUGETLB_1G_FREE "$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages 2>/dev/null)"
kv ULIMIT_MEMLOCK "$(ulimit -l)"; kv ULIMIT_NOFILE "$(ulimit -n)"
# userfaultfd (libfabric's uffd MR cache monitor): the syscall must work for an unprivileged process
kv UFFD_UNPRIV_SYSCTL "$(cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null)"
kv UFFD_SYSCALL "$(python3 -I -c 'import ctypes,os
l=ctypes.CDLL(None,use_errno=True)
for nm,fl in (("plain",0),("user_mode_only",1)):
    fd=l.syscall(323, os.O_CLOEXEC|os.O_NONBLOCK|fl)
    print("%s:%s"%(nm,"works" if fd>=0 else "fails_errno=%d"%ctypes.get_errno()),end=" ")' 2>&1 | head -1)"
# APUs
kv ROCM_SMI_PRESENT "$(command -v rocm-smi >/dev/null && echo yes || echo no)"
if command -v rocm-smi >/dev/null 2>&1; then
  kv APU_COUNT "$(rocm-smi --showid 2>/dev/null | grep -c 'Device Name')"
  kv APU_NAMES "$(rocm-smi --showproductname 2>/dev/null | grep -i 'Card Series' | sed 's/.*: *//' | sort | uniq -c | paste -sd';' | tr -s ' ')"
  kv APU_VRAM_TOTAL_B "$(rocm-smi --showmeminfo vram 2>/dev/null | grep 'Total Memory' | awk '{print $NF}' | paste -sd' ')"
  kv APU_GTT_TOTAL_B "$(rocm-smi --showmeminfo gtt 2>/dev/null | grep 'Total Memory' | awk '{print $NF}' | paste -sd' ')"
  kv APU_PARTITION "$(rocm-smi --showcomputepartition 2>/dev/null | grep -i 'partition' | sed 's/.*: *//' | sort | uniq -c | paste -sd';' | tr -s ' ')"
fi
kv APU_PCI_COUNT "$(for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor 2>/dev/null)" = 0x1002 ] && case "$(cat $d/class 2>/dev/null)" in 0x030000|0x038000|0x120000) echo x;; esac; done | wc -l)"
m=""; for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor 2>/dev/null)" = 0x1002 ] && case "$(cat $d/class 2>/dev/null)" in 0x030000|0x038000|0x120000) m="$m numa$(cat $d/numa_node)";; esac; done; kv APU_NUMA_MAP "${m# }"
kv HIP_RUNTIME_LIBS "$(ls /opt/rocm*/lib/libamdhip64.so.* 2>/dev/null | head -3 | paste -sd' ')"
[ -r /opt/rocm/.info/version ] && kv ROCM_VERSION_FILE "$(cat /opt/rocm/.info/version)"
kv ROCM_DIRS "$(ls -d /opt/rocm* 2>/dev/null | paste -sd' ')"
# NICs
kv CXI_DEV_COUNT "$(ls -d /sys/class/cxi/cxi[0-9]* 2>/dev/null | wc -l)"
m=""; for d in /sys/class/cxi/cxi*; do [ -e $d ] && m="$m $(basename $d):numa$(cat $d/device/numa_node 2>/dev/null)"; done; kv CXI_NUMA_MAP "${m# }"
kv CXI_LINK_STATE "$(for d in /sys/class/net/hsn*; do [ -e $d ] && echo "$(basename $d)=$(cat $d/operstate 2>/dev/null)"; done | paste -sd' ')"
kv CXI_LINK_SPEED_MBPS "$(for d in /sys/class/net/hsn*; do [ -e $d ] && cat $d/speed 2>/dev/null; done | sort | uniq -c | paste -sd';' | tr -s ' ')"
if command -v fi_info >/dev/null 2>&1; then
  kv FI_INFO_PATH "$(command -v fi_info)"; kv FI_VERSION "$(fi_info --version 2>&1 | grep -i 'libfabric:' | head -1)"
  kv FI_PROVIDERS "$(fi_info -l 2>/dev/null | grep -E '^[a-z_;:+]+:' | tr -d ':' | paste -sd' ')"
  kv FI_CXI_DOMAINS "$(fi_info -p cxi 2>/dev/null | grep -c 'domain:')"
  kv FI_CXI_DOMAIN_NAMES "$(fi_info -p cxi 2>/dev/null | grep 'domain:' | sort -u | awk '{print $2}' | paste -sd' ')"
  for mon in userfaultfd memhooks kdreg2 cuda rocr disabled; do
    r=$(FI_MR_CACHE_MONITOR=$mon timeout 20 fi_info -p cxi 2>&1 | head -3 | tr '\n' ' ' | cut -c1-100); case "$r" in *provider:*) s=accepted;; *) s="refused/other: $r";; esac; kv "FI_MONITOR_$mon" "$s"; done
else kv FI_INFO_PATH none; fi
kv LIBFABRIC_DIRS "$(ls -d /opt/cray/libfabric/* 2>/dev/null | paste -sd' ')"
kv XPMEM_DEV "$(ls -l /dev/xpmem 2>/dev/null | wc -l) (1 = /dev/xpmem exists)"; kv XPMEM_MODULE "$(grep -c '^xpmem' /proc/modules 2>/dev/null)"
kv KNEM_DEV "$(ls /dev/knem 2>/dev/null | wc -l)"; kv CMA_PTRACE_SCOPE "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)"
# file systems (candidates for the output directory): type, size, free; the single-file write rate is measured by the runbook (dd/ecalc), not here
df -PT -x tmpfs -x devtmpfs -x squashfs -x overlay 2>/dev/null | awk 'NR>1 && $3>1e8 {printf "FS_CANDIDATE=%s %s size_kB=%s avail_kB=%s\n",$7,$2,$3,$5}' | head -12
NODE
}
node_phase() {
  sec "node ($([ -n "$HOLD" ] && echo hold node || echo local))"
  node_phase_script > "$OUT/node_facts.sh"
  { echo 'kv() { printf "%s=%s\n" "$1" "$2"; }'; sed '1d' "$OUT/node_facts.sh"; } > "$OUT/node_facts_run.sh"
  local pre=""; [ -n "${PICK_MODULES_ENV:-}" ] && pre="module load $PICK_MODULES_ENV;"
  nrun bash "$OUT/node_facts_run.sh" 2>&1 | scrub >> "$F"
  [ -n "$HOLD" ] && kv NODE_FACTS_FROM "hold node (srun)" || kv NODE_FACTS_FROM "this machine (a login node shows the login node, not a compute node)"
}

# ---- Slurm --------------------------------------------------------------------------------------------------------------------------
slurm_phase() {
  sec slurm
  command -v sinfo >/dev/null || { kv SLURM none; return; }
  kv SLURM_VERSION "$(T=15 run sinfo --version)"
  T=20 run sinfo -h -o 'PARTITION=%P nodes=%D cpus=%c mem_MB=%m timelimit=%l gres=%G state=%a' | sort -u | head -12 | scrub >> "$F"
  kv SLURM_DEFAULT_PARTITION "$(T=20 run sinfo -h -o '%P' | grep '\*' | tr -d '*' | head -1)"
  T=20 run scontrol show config | grep -E '^(SelectType|SelectTypeParameters|MaxJobCount|PrologFlags|ProctrackType|TaskPlugin|SchedulerType|DefMemPerNode|MpiDefault|SlurmVersion|SrunPortRange|TopologyPlugin|GresTypes)' | sed 's/  */ /g;s/^/CFG_/;s/ = /=/' | scrub >> "$F"
  kv QOS "$(T=25 run sacctmgr -nP show qos format=name,maxtrespu,maxwall,maxjobspu,maxsubmitpu,priority | paste -sd';')"
  kv ASSOC "$(T=25 run sacctmgr -nP show assoc where user="$UN" format=account,partition,qos,maxtrespu,maxjobs,maxsubmit,grptres | sort -u | head -6 | paste -sd';')"
  kv SQUEUE_OWN_JOBS "$(T=15 run squeue -h -u "$UN" | wc -l)"
}

# ---- SHMEM (build, run on one node) -----------------------------------------------------------------------------------------------
shmem_phase() {
  sec shmem
  [ "$DO_SHMEM" = 1 ] || { kv SHMEM_TESTS skipped; return; }
  init_modules || { kv SHMEM_TESTS no_module_command; return; }
  local mods; mods=${PROBE_MODULES:-$PICK_MODULES}; [ -n "$mods" ] || { kv SHMEM_TESTS no_modules_picked; return; }
  # a conflicting rocm family: unload what is loaded, then load the picked line; the probe runs in this shell only (a subshell below)
  local cc; cc=$(command -v cc || command -v gcc)
  [ -n "$cc" ] || { kv SHMEM_BUILD no_c_compiler; return; }
  local loaded; loaded=$(module list 2>&1 | grep -oE '(rocm|amd)/[0-9][A-Za-z0-9_.+-]*' | tr '\n' ' ')
  (
    for m in $loaded; do module unload "$m" >/dev/null 2>&1; done
    module load $mods > "$OUT/module_load.log" 2>&1; echo "rc=$?" >> "$OUT/module_load.log"
    kv MODULE_LOAD_RC "$(tail -1 "$OUT/module_load.log")"
    kv MODULES_AFTER_LOAD "$(module list 2>&1 | grep -oE '[A-Za-z0-9_.+-]+/[0-9][A-Za-z0-9_.+-]*' | paste -sd' ')"
    sma=${CRAY_OPENSHMEMX_PREFIX:-$(ls -d /opt/cray/pe/sma/*/ofi/sma 2>/dev/null | sort -V | tail -1)}; dsm=${CRAY_DSMML_PREFIX:-$(ls -d /opt/cray/pe/dsmml/*/dsmml 2>/dev/null | sort -V | tail -1)}
    kv SMA_PREFIX "$sma"; kv DSMML_PREFIX "$dsm"
    kv SMA_LIB_NEEDED "$(ldd "$sma"/lib64/libsma.so 2>/dev/null | grep -oE 'lib(xpmem|fabric|cxi|hsa-runtime64|amdhip64|pmi2?|ze_loader|cuda)[^ ]*' | sort -u | paste -sd' ')"
    kv SMA_LIBFABRIC_RESOLVED "$(ldd "$sma"/lib64/libsma.so 2>/dev/null | awk '/libfabric/{print $3}' | head -1)"
    kv SMA_VERSION_DIRS "$(ls -d /opt/cray/pe/sma/*/ 2>/dev/null | xargs -n1 basename 2>/dev/null | paste -sd' ')"
    if $cc -O1 -o "$OUT/probe_shmem" "$HERE/target_probe_shmem.c" -I"$sma"/include -I"$dsm"/include -L"$sma"/lib64 -L"$dsm"/lib -L/opt/cray/pe/lib64 \
         -lsma -lpmi -lpmi2 -ldsmml -ldl -Wl,-rpath,"$sma"/lib64 -Wl,-rpath,"$dsm"/lib -Wl,-rpath,/opt/cray/pe/lib64 > "$OUT/probe_build.log" 2>&1; then kv SHMEM_BUILD ok
    else kv SHMEM_BUILD "FAILED: $(head -c 200 "$OUT/probe_build.log" | tr '\n' ' ')"; exit 0; fi
    [ -n "$HOLD" ] || { kv SHMEM_RUN "not run (no --hold: a login node has no PMI step; run with --hold <job> on a compute node)"; exit 0; }
    # one PE, then 2 PEs of one node; heap 1 GiB.  Both launches are plain srun, as mnrun.sh does.
    export SHMEM_SYMMETRIC_SIZE=1073741824 XT_SYMMETRIC_HEAP_SIZE=1073741824
    shm_case() { # <label> <npes> [VAR=val ...]: one SHMEM run of the test; prints bounded PROBE lines, the rc and the first error line
      local lab=$1 n=$2; shift 2; local o rc
      o=$(env "$@" timeout "$TMO" srun --jobid="$HOLD" -N1 -n"$n" --ntasks-per-node="$n" --overlap --mem=0 ${PROBE_SRUN_EXTRA:-} "$OUT/probe_shmem" 2>&1 < /dev/null); rc=$?
      kv "SHMEM_${lab}_RC" "$rc"
      kv "SHMEM_${lab}_PROBE" "$(echo "$o" | grep -a '^PROBE' | sed 's/^PROBE //' | sort -u | head -6 | paste -sd'|')"
      [ "$rc" = 0 ] || kv "SHMEM_${lab}_ERROR" "$(echo "$o" | grep -aiE 'error|abort|segm|invalid|only [0-9]+ nic|fatal' | head -2 | cut -c1-220 | paste -sd'|')"
    }
    shm_case ONEPE 1
    shm_case TWOPE 2
    shm_case TWOPE_LOCALITY_ONNODE 2 SHMEM_LOCALITY_ONNODE=1          # segfaulted on aac7 (B2 probe): a crash here is an answer, not a probe failure
    for n in 1 2 4 8; do shm_case "NICS$n" 1 SHMEM_OFI_NUM_NICS=$n SHMEM_OFI_NIC_POLICY=ROUND-ROBIN; done
    for p in BLOCK NUMA ROUND-ROBIN; do shm_case "POLICY_$p" 1 SHMEM_OFI_NUM_NICS=2 SHMEM_OFI_NIC_POLICY=$p; done
    shm_case "NICS2_4PE" 4 SHMEM_OFI_NUM_NICS=2 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
    shm_case "NICS4_5PE" 5 SHMEM_OFI_NUM_NICS=4 SHMEM_OFI_NIC_POLICY=ROUND-ROBIN
  ) 
}

# ---- the proposal -----------------------------------------------------------------------------------------------------------------
fact() { sed -n "s/^$1=//p" "$F" | head -1; }
propose() {
  local P=$OUT/target.conf.proposed; cp "$TPL" "$P"
  setk() { # key value comment: replace "KEY=__FILL__" by the value (comment kept, "(probe)" tag appended); empty value leaves __FILL__
    [ -n "$2" ] || return 0; sed -i "s|^$1=__FILL__\( *\)#\?|$1=$2\1# (probe $(date +%F)) |" "$P"; }
  local r s d mods memav node_gb dev_gb partition
  r=$(fact PICK_ROCM); s=$(fact PICK_SMA); d=$(fact PICK_DSMML); case "$r" in NONE*) r=;; esac; case "$s" in NONE*) s=;; esac; case "$d" in NONE*) d=;; esac
  setk TARGET_ROCM "$r"; setk TARGET_SMA_MODULE "$s"; setk TARGET_DSMML_MODULE "$d"
  [ -n "$r" ] && [ -n "$s" ] && [ -n "$d" ] && setk MNRUN_MODULES "\"$d $s $r\""
  # unload list: every rocm/amd module loaded by default (a versioned rocm conflicts with it); empty = none loaded
  local ul; ul=$(fact MOD_LOADED_ROCM); [ -n "$ul" ] && setk MNRUN_UNLOAD "\"$ul\"" || setk MNRUN_UNLOAD '""'
  if [ -f /usr/include/gmp.h ] || [ -n "${CRAY_GMP_PREFIX:-}" ]; then sed -i '/^GMP_HOME=__FILL__/s|^|# (probe: a gmp.h is available; drop the key) |' "$P"; fi
  partition=$(fact SLURM_DEFAULT_PARTITION); setk TARGET_PARTITION "$partition"
  memav=$(fact MEM_MemAvailable); memav=${memav% kB}
  if [ -n "$memav" ] && fact NODE_FACTS_FROM | grep -q '^hold'; then node_gb=$(( memav / 1048576 - 4 )); [ "$node_gb" -gt 0 ] && setk ECALC_NODE_GB "$node_gb"; fi
  setk MNRUN_EXCLUDE_HOSTS '""'
  # keys that stay for a human (site choices): the output directory and scratch
  # advisories as comments at the end
  { echo; echo "# ---- probe advisories ($(date +%F)); nothing below is read by a script ----"
    echo "# APUs: $(fact APU_COUNT) (pci $(fact APU_PCI_COUNT)), VRAM B: $(fact APU_VRAM_TOTAL_B); ECALC_DEVICE_GB template 373.44 = 4 x 93.36 GB (m, D26): confirm with the t_edge test of the runbook"
    echo "# NICs: cxi devices $(fact CXI_DEV_COUNT), fi_info cxi domains $(fact FI_CXI_DOMAINS), numa map: $(fact CXI_NUMA_MAP); APU numa map: $(fact APU_NUMA_MAP)"
    echo "# uffd syscall: $(fact UFFD_SYSCALL); FI_MR_CACHE_MONITOR=userfaultfd is $(fact FI_MONITOR_userfaultfd) here"
    echo "# on-node path: /dev/xpmem=$(fact XPMEM_DEV); 2-PE shmem_ptr/put/get: $(fact SHMEM_TWOPE_PROBE)"
    echo "# COMM_SHMEM_PE_PER_APU advice: $(advice)"
    echo "# ECALC_OUT_DIR / MNACCEPT_TMP: pick a shared, non-RAM directory from the FS_CANDIDATE lines of facts.txt; the single-file write rate comes from the runbook step 'output FS'"
  } >> "$P"
}
advice() {  # the SHMEM facts that decide COMM_SHMEM_PE_PER_APU (see profiles/README.md runbook step 9)
  local nics n2 n4 two
  n2=$(fact SHMEM_NICS2_RC); n4=$(fact SHMEM_NICS4_RC); two=$(fact SHMEM_TWOPE_PROBE)
  [ -n "$n2" ] || { echo "UNKNOWN (SHMEM runs not done: rerun with --hold on a compute node)"; return; }
  if [ "$n4" = 0 ]; then echo "one PE accepts SHMEM_OFI_NUM_NICS=4 -> one PE can use >1 NIC; try PE_PER_APU=0 with NUM_NICS=$(fact CXI_DEV_COUNT) first (measure), then 1"
  elif [ "$n2" = 0 ]; then echo "one PE accepts NUM_NICS=2 but not 4 -> 1 PE per APU gets both of its NICs (PE_PER_APU=1 is the 2-NIC form); a value 2 would need the NIC count per PE >2 (not needed)"
  else echo "one PE accepts only 1 NIC -> use PE_PER_APU=2 (two PEs per APU, one per NIC) if the 2 NICs per APU must both be used; PE_PER_APU=1 uses one NIC per APU"; fi
  case "$two" in *"nonnull=0/"*) echo "# NOTE: shmem_ptr is NULL for a same-node peer: no on-node path, the proxy mode needs the /dev/shm heap workaround (N7b)";; esac
}
report() {
  echo "=== target_probe: facts ($F) ==="
  grep -v '^####' "$F" | cut -c1-230 | head -${PROBE_MAXLINES:-140}
  echo "=== proposed target.conf: $OUT/target.conf.proposed (diff against the template) ==="
  diff <(grep -v '^#' "$TPL" | grep '=') <(grep -v '^#' "$OUT/target.conf.proposed" | grep '=') | cut -c1-200 | head -40
  echo "=== still __FILL__ ===" ; grep -c '^[A-Z_]*=__FILL__' "$OUT/target.conf.proposed" | tr '\n' ' '; grep -o '^[A-Z_]*=__FILL__' "$OUT/target.conf.proposed" | cut -d= -f1 | paste -sd' '
}

case $PHASE in
  all)   modules_phase; slurm_phase; node_phase; shmem_phase; propose; report | scrub;;
  login) modules_phase; slurm_phase;;
  node)  node_phase;;
  shmem) modules_phase; shmem_phase;;
  *) echo "target_probe: --phase all|login|node|shmem" >&2; exit 2;;
esac
exit 0
