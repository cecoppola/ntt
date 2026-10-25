#!/bin/bash
# items/_build.sh VARIANT - shared body of bld01 (p1) and bld02 (p2); run as `bash -l _build.sh p1|p2` so the module command exists.
#   p1: default build, XSHMEM=ofi (libfabric cxi, PMI2)       -> build/<sha7>-p1/ecalc/ecalc
#   p2: make XSHMEM=cray (Cray SHMEM backend, -lsma)          -> build/<sha7>-p2/ecalc/ecalc
# The tree is a copy of ecalc/ and tools/ (so p1 and p2 can build at the same time and mnrun.sh finds its own ecalc next to it).
# Modules: MNRUN_MODULES / MNRUN_UNLOAD from config/target.conf (or the environment) are tried first; when that is unset or fails, the
# candidates of the kit's build stage (ecalc/target_kit.sh stage_build): the loaded rocm, the validated list rocm/7.2.4 7.2.3 7.0.3,
# then the other rocm/<version> newest first (aac7: 7.12-7.14 failed to link ecalc with an -fPIC device-link error, so a failing
# newest ROCm is expected to fall back and the result says which one linked).  Env: BLD_JOBS (8), BLD_TESTS (extra test binaries),
# TARGET_ROCM / TARGET_SMA_MODULE / TARGET_DSMML_MODULE (as in the kit), GMP_HOME.
# Summary keys: e.binary (relative to target/), e.build_dir, e.MNRUN_MODULES, e.MNRUN_UNLOAD, e.rocm, e.modules, e.hipcc, e.libs,
# m.build_s (total), m.make_s (the winning make), m.candidates_tried.
V=${1:?variant p1|p2}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$here/../../lib/common.sh"
tgt_init
: "${OUT:?run through bench/run.sh}"; export TGT_SUM_DIR=${TGT_SUM_DIR:-$OUT}
LOG=$OUT/raw/build.log; : > "$LOG"
T0=$(date +%s)
case $V in
    p1) XS=ofi;  XTESTS=${BLD_TESTS-tests/t_xshmem tests/t_pe_geo};;
    p2) XS=cray; XTESTS=${BLD_TESTS-tests/t_xshmem};;
    *) echo "variant p1|p2" >&2; exit 2;;
esac
JOBS=${BLD_JOBS:-8}
B=$TGT_ROOT/build/$TGT_SHA-$V
say() { echo "$*" | tee -a "$LOG"; }
fail() { tgt_kv note "$*"; say "FAIL: $*"; exit 1; }

# ---- sources
mkdir -p "$B" || fail "cannot create $B"
rm -rf "$B/ecalc" "$B/tools"
tar -C "$REPO" -cf - --exclude='*.o' --exclude=ecalc/ecalc ecalc tools 2>/dev/null | tar -C "$B" -xf - || fail "copying sources to $B failed"
[ -f "$B/ecalc/Makefile" ] || fail "no Makefile in the copy"
say "== $V XSHMEM=$XS tree $B, make -j$JOBS, extra tests: ${XTESTS:-none}"

# ---- the module command
if ! type module >/dev/null 2>&1; then
    for f in /usr/share/lmod/lmod/init/bash /opt/cray/pe/lmod/lmod/init/bash /usr/share/Modules/init/bash /etc/profile.d/modules.sh; do [ -f "$f" ] && . "$f" && break; done
fi
HAVE_MOD=0; type module >/dev/null 2>&1 && HAVE_MOD=1
say "module command: $HAVE_MOD"

# ---- candidates: lines "UNLOAD|LOAD"
cur_rocm=
[ $HAVE_MOD = 1 ] && cur_rocm=$(module -t list 2>&1 | grep -oE '^rocm/[0-9][0-9.]*|^rocm$' | tr '\n' ' ')
declare -a CANDS=()
if [ -n "${MNRUN_MODULES:-}" ]; then CANDS+=("${MNRUN_UNLOAD:-}|$MNRUN_MODULES"); fi
if [ $HAVE_MOD = 1 ]; then
    sma=${TARGET_SMA_MODULE:-$(module -t avail cray-openshmemx 2>&1 | grep -oE '^cray-openshmemx/[0-9][0-9.]*' | sort -V | tail -1)}
    dsm=${TARGET_DSMML_MODULE:-$(module -t avail cray-dsmml 2>&1 | grep -oE '^cray-dsmml/[0-9][0-9.]*' | sort -V | tail -1)}
    [ -n "$sma" ] || sma=cray-openshmemx; [ -n "$dsm" ] || dsm=cray-dsmml
    pre=; [ $V = p2 ] && pre="$dsm $sma "
    avail=$(module -t avail rocm 2>&1 | grep -oE '^rocm/[0-9][0-9.]*' | sort -Vu)
    declare -a R=()
    if [ -n "${TARGET_ROCM:-}" ]; then R=("$TARGET_ROCM")
    else
        [ -n "$cur_rocm" ] && R+=("$(echo $cur_rocm | awk '{print $1}')")
        for v in rocm/7.2.4 rocm/7.2.3 rocm/7.0.3; do echo "$avail" | grep -qx "$v" && R+=("$v"); done
        for o in $(echo "$avail" | sort -Vr); do R+=("$o"); done
    fi
    declare -A seen=()
    for r in "${R[@]}"; do [ -n "${seen[$r]:-}" ] && continue; seen[$r]=1; CANDS+=("$cur_rocm|$pre$r"); done
fi
CANDS+=("|")      # as is: whatever the shell has (no module command, or the last resort)
say "candidates: ${#CANDS[@]}"

build_one() {      # UNLOAD LOAD -> rc; in a subshell so the modules do not leak
    local ul=$1 ld=$2 rc
    (
        if [ $HAVE_MOD = 1 ]; then
            for x in $ul; do module unload "$x" >/dev/null 2>&1; done
            [ -n "$ld" ] && { module load $ld >> "$LOG" 2>&1 || exit 97; }
        fi
        command -v hipcc >/dev/null || { echo "no hipcc after loading: $ld" >> "$LOG"; exit 98; }
        cd "$B/ecalc" || exit 99
        [ -n "${GMP_HOME:-}" ] && GM="GMP_HOME=$GMP_HOME"
        make clean >> "$LOG" 2>&1
        make -j"$JOBS" XSHMEM=$XS $GM ecalc tools tests/t_edge tests/t_nodechk >> "$LOG" 2>&1 || exit 1
        [ -x ecalc ] && [ -x tests/t_edge ] || exit 1
        EX=0
        for t in $XTESTS; do make -j"$JOBS" XSHMEM=$XS $GM "$t" >> "$LOG" 2>&1 && [ -x "$t" ] || { echo "extra test $t FAILED" >> "$LOG"; EX=1; }; done
        [ $EX = 0 ] || exit 3
        exit 0
    )
}

tried=0; chosen=; rc=1
for c in "${CANDS[@]}"; do
    ul=${c%%|*}; ld=${c#*|}; tried=$((tried + 1))
    say "== candidate $tried: unload [${ul:-}] load [${ld:-as is}]"
    t1=$(date +%s)
    build_one "$ul" "$ld"; rc=$?
    say "== candidate $tried rc=$rc ($(( $(date +%s) - t1 )) s)"
    if [ $rc = 0 ] || [ $rc = 3 ]; then chosen=$c; MAKE_S=$(( $(date +%s) - t1 )); break; fi
done
tgt_kv m.candidates_tried "$tried"
tgt_kv m.build_s "$(( $(date +%s) - T0 )) s"
[ -n "$chosen" ] || fail "no module candidate built (tried $tried; see raw/build.log; error lines: $(grep -m3 -E 'error|Error' "$LOG" | tr '\n' ' ' | cut -c1-300))"
tgt_kv m.make_s "$MAKE_S s"
ul=${chosen%%|*}; ld=${chosen#*|}
BIN=$B/ecalc/ecalc

# ---- what was built
sig=$( (readelf -d "$BIN" 2>/dev/null | grep -oE 'lib(sma|fabric|pmi2?|dsmml|amdhip64)[^ ]*' | sort -u | tr '\n' ' ') )
case $V in
    p2) echo "$sig" | grep -q libsma || fail "p2 binary is not linked with libsma (XSHMEM=cray not in effect): $sig";;
    p1) echo "$sig" | grep -q libfabric || fail "p1 binary is not linked with libfabric (XSHMEM=ofi not in effect): $sig"
        echo "$sig" | grep -q libsma && tgt_kv note "p1 binary also links libsma";;
esac
if [ $HAVE_MOD = 1 ] && [ -n "$ld" ]; then mods=$( (for x in $ul; do module unload "$x" >/dev/null 2>&1; done; module load $ld >/dev/null 2>&1; module -t list 2>&1 | grep -v '^$' | grep -v ':$' | tr '\n' ' ') ); fi
hv=$( (for x in $ul; do module unload "$x" >/dev/null 2>&1; done; [ -n "$ld" ] && module load $ld >/dev/null 2>&1; hipcc --version 2>&1 | head -1) )
nf=$( (for x in $ul; do module unload "$x" >/dev/null 2>&1; done; [ -n "$ld" ] && module load $ld >/dev/null 2>&1; ldd "$BIN" 2>/dev/null | grep -c 'not found') )
tgt_kv e.binary "build/$TGT_SHA-$V/ecalc/ecalc"
tgt_kv e.build_dir "build/$TGT_SHA-$V"
tgt_kv e.MNRUN_MODULES "$ld"
tgt_kv e.MNRUN_UNLOAD "$ul"
tgt_kv e.rocm "$(echo "$ld" | grep -oE 'rocm[^ ]*' | head -1)"
tgt_kv e.modules "${mods:-none}"
tgt_kv e.hipcc "$hv"
tgt_kv e.libs "$sig"
tgt_kv e.xshmem "$XS"
tgt_kv m.unresolved_libs "${nf:-0}"
[ "${nf:-0}" = 0 ] || tgt_kv note "ldd reports $nf unresolved libs on this node (the compute nodes may differ)"
[ $rc = 3 ] && { tgt_kv status PARTIAL; tgt_kv note "ecalc built; some extra test binaries failed (see raw/build.log)"; }
say "built $BIN with [$ld] in $(( $(date +%s) - T0 )) s"
exit 0
