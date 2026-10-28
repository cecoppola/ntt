#!/bin/bash
# s2_build.sh - stage 2: build ecalc for path 1 (libfabric, XSHMEM=ofi) and/or path 2 (Cray SHMEM, XSHMEM=cray) into separate build trees.
#   s2_build.sh [--path 1|2|both] [--jobs 32] [--node] [--jobid J] [--no-gmp] [--dry-run]
# Wraps target/bench/items/_build.sh (module candidates, copy of ecalc/ + tools/ into target/build/<sha>-p1|p2/, checks that the binary links
# libfabric / libsma, builds the test binaries t_edge t_nodechk t_xshmem t_pe_geo and tools/unpack_digits).  Default builds on this (login)
# node, both paths at once; --node builds on the first node of the allocation (--jobid / REMOTE_JOB) when the login node has no compiler.
# GMP: GMP_HOME or a system gmp.h is used; otherwise a source tarball (gmp-*.tar.* next to the repo, in $REMOTE_OUT or $HOME) or a download
# (REMOTE_GMP_URL=<link>) is built into $REMOTE_OUT/gmp (--no-gmp: do not try); without GMP the build goes on (nothing it builds needs it).  Writes REMOTE_ECALC1/2, REMOTE_MODULES_P1/P2, REMOTE_UNLOAD, GMP_HOME
# into site.env.  Verdict: PASS all requested built, PARTIAL with --path both when exactly one path built (carry on with that path: chain.sh goes on, s3_gate.sh skips the missing path), FAIL otherwise.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"; set -u
PATHS=both JOBS=32 NODE=0 JOB= NOGMP=0
while [ $# -gt 0 ]; do case $1 in
  --path) PATHS=${2:?}; shift;; --jobs) JOBS=${2:?}; shift;; --node) NODE=1;; --jobid) JOB=${2:?}; shift;; --no-gmp) NOGMP=1;; --dry-run) DRY=1;;
  -h|--help) rl_help "$0"; exit 0;; *) echo "s2_build.sh: unknown option $1 (--help)" >&2; exit 2;; esac; shift; done
case $PATHS in 1|2|both) ;; *) echo "s2_build.sh: --path 1|2|both" >&2; exit 2;; esac
rl_posint "$JOBS" || { echo "s2_build.sh: --jobs N (a positive integer)" >&2; exit 2; }
JOBS=$((10#$JOBS))
rl_init s2
rl_conf
BUILD=$R/target/bench/items/_build.sh
want=(); case $PATHS in 1) want=(1);; 2) want=(2);; both) want=(1 2);; esac
if [ "$DRY" = 1 ]; then
    rl_would "gmp: ensure \$GMP_HOME/include/gmp.h or a system gmp.h; else build GMP 6.3.0 from a tarball into $REMOTE_OUT/gmp"
    for p in "${want[@]}"; do rl_would "OUT=$RUN/bld_p$p BLD_JOBS=$JOBS MNRUN_MODULES=\"$(rl_mods "$p")\" MNRUN_UNLOAD=\"$(rl_unload)\" GMP_HOME=${GMP_HOME:-<unset>} bash -l $BUILD p$p   # make XSHMEM=$([ $p = 1 ] && echo ofi || echo cray)$([ $NODE = 1 ] && echo '; on a node of the allocation')"; done
    rl_would "site.env <- REMOTE_ECALC1/2 REMOTE_MODULES_P1/P2 REMOTE_UNLOAD GMP_HOME"
    rl_finish SKIP "$HERE/s2_build.sh --path $PATHS"
fi
# ---- GMP
gmp_ok() { [ -n "${GMP_HOME:-}" ] && [ -f "$GMP_HOME/include/gmp.h" ] && return 0; [ -z "${GMP_HOME:-}" ] && { [ -f /usr/include/gmp.h ] || [ -f /usr/include/x86_64-linux-gnu/gmp.h ]; } && return 0; return 1; }
gmp_build() {
    local P=$REMOTE_OUT/gmp tb d
    tb=$(ls "$R"/../gmp-*.tar.* "$REMOTE_OUT"/gmp-*.tar.* "$HOME"/gmp-*.tar.* 2>/dev/null | head -1)
    if [ -z "$tb" ]; then
        d=$REMOTE_OUT/gmp-6.3.0.tar.xz
        # no address is stored here: set REMOTE_GMP_URL=<link to gmp-6.3.0.tar.xz> to let this script download it
        if [ -n "${REMOTE_GMP_URL:-}" ]; then
            if rl_have curl; then rl_tmo 150 curl -fsSL -o "$d" "$REMOTE_GMP_URL" >> "$RUN/raw/gmp.log" 2>&1
            elif rl_have wget; then rl_tmo 150 wget -q -O "$d" "$REMOTE_GMP_URL" >> "$RUN/raw/gmp.log" 2>&1; fi
        fi
        [ -s "$d" ] && tb=$d
    fi
    [ -n "$tb" ] || { echo "no GMP tarball (put gmp-6.3.0.tar.xz into $REMOTE_OUT or set REMOTE_GMP_URL)" >> "$RUN/raw/gmp.log"; return 1; }
    mkdir -p "$RUN/gmpsrc" && tar -C "$RUN/gmpsrc" -xf "$tb" >> "$RUN/raw/gmp.log" 2>&1 || return 1
    ( cd "$RUN"/gmpsrc/gmp-* && rl_tmo 1500 ./configure --prefix="$P" --enable-fat >> "$RUN/raw/gmp.log" 2>&1 && rl_tmo 1500 make -j"$JOBS" >> "$RUN/raw/gmp.log" 2>&1 && rl_tmo 600 make install >> "$RUN/raw/gmp.log" 2>&1 ) || return 1
    rm -rf "$RUN/gmpsrc"; [ -f "$P/include/gmp.h" ] || return 1
    rl_site_set GMP_HOME "$P"; return 0
}
rl_errfiles "$RUN/raw/gmp.log"
# the site gmp module sets GMP_BASE (not GMP_HOME): use it when GMP_HOME is unset and it holds the header
if [ -z "${GMP_HOME:-}" ] && [ -n "${GMP_BASE:-}" ] && [ -f "$GMP_BASE/include/gmp.h" ]; then GMP_HOME=$GMP_BASE; rl_say "GMP_HOME <- GMP_BASE=$GMP_BASE (gmp module)"; fi
if gmp_ok; then rl_kv gmp "${GMP_HOME:-system}"
elif [ $NOGMP = 1 ]; then rl_kv gmp "none (--no-gmp)"
else
    rl_say "no GMP found: building GMP into $REMOTE_OUT/gmp"
    if gmp_build; then rl_kv gmp "built: $GMP_HOME"; else unset GMP_HOME; rl_kv gmp "none (not needed: ecalc, tools, t_edge, t_nodechk, t_xshmem, t_pe_geo build without it; only the unit tests and ref/gen_e need GMP)"; fi
fi
[ -n "${GMP_HOME:-}" ] && export GMP_HOME
# ---- builds
wrap() {   # PATH: one build, output to $RUN/bld_pN/
    local p=$1 o=$RUN/bld_p$1 mods; mkdir -p "$o/raw"; : > "$o/summary.txt"
    mods=$(rl_mods "$p")
    local envs=(OUT="$o" TGT_SUM_DIR="$o" BLD_JOBS="$JOBS" MNRUN_MODULES="$mods" MNRUN_UNLOAD="$(rl_unload)")
    [ -n "${GMP_HOME:-}" ] && envs+=(GMP_HOME="$GMP_HOME")
    if [ $NODE = 1 ]; then
        local h; h=$(rl_pick_hosts "$JOB" 1) || { echo "no usable node in job $JOB" > "$o/build.out"; return 1; }
        rl_tmo 3000 srun --jobid="$JOB" -N1 -n1 -w "$h" --overlap --export=ALL -c 64 env "${envs[@]}" bash -l "$BUILD" "p$p" > "$o/build.out" 2>&1 < /dev/null
    else
        rl_tmo 3000 env "${envs[@]}" bash -l "$BUILD" "p$p" > "$o/build.out" 2>&1 < /dev/null
    fi
}
if [ $NODE = 1 ]; then rl_resolve_job || rl_die "--node needs --jobid or REMOTE_JOB"; rl_need_running "$JOB"; fi
pids=(); for p in "${want[@]}"; do rl_say "building path $p (make -j$JOBS)"; wrap "$p" & pids+=($!); done
rcs=(); i=0; for pid in "${pids[@]}"; do wait "$pid"; rcs[$i]=$?; i=$((i + 1)); done
ok1=0 ok2=0 i=0
loaded_mods() {   # PATH -> the modules of the build environment that matter for a launch (target mode, when the winning candidate was "as is")
    local lm
    if [ $NODE = 1 ]; then lm=$(rl_tmo 60 srun --jobid="$JOB" -N1 -n1 --overlap --export=ALL bash -lc 'module -t list 2>&1' < /dev/null 2>/dev/null)
    else lm=$(rl_tmo 30 bash -lc 'module -t list 2>&1' < /dev/null 2>/dev/null); fi
    echo "$lm" | grep -a -E '^(rocm|cray-openshmemx|cray-shmem|cray-sma|cray-dsmml|libfabric)(/|$)' | { if [ "$1" = 1 ]; then grep -v -E '^cray-(dsmml|openshmemx|shmem|sma)'; else cat; fi; } | paste -sd' '
}
sv() { grep -a -m1 "^$2=" "$RUN/bld_p$1/summary.txt" 2>/dev/null | sed 's/^[^=]*=//'; }
rl_line "builds:"
for p in "${want[@]}"; do
    o=$RUN/bld_p$p; rc=${rcs[$i]}; i=$((i + 1))
    rl_errfiles "$o/raw/build.log" "$o/build.out"
    bin=$(sv "$p" e.binary); flags=$(grep -a -o -E '\-DXSHMEM_(OFI|CRAY)' "$o/raw/build.log" 2>/dev/null | sort -u | paste -sd' ')
    if [ $rc = 0 ] && [ -n "$bin" ] && [ -x "$R/target/$bin" ]; then
        abs=$R/target/$bin; st=$(sv "$p" status)
        rl_kv "path$p.binary" "$abs"; rl_kv "path$p.flags" "${flags:-none seen}"; rl_kv "path$p.libs" "$(sv "$p" e.libs)"
        rl_kv "path$p.modules" "$(sv "$p" e.MNRUN_MODULES)"; rl_kv "path$p.rocm" "$(sv "$p" e.rocm)"; rl_kv "path$p.build_s" "$(sv "$p" m.build_s)"; rl_kv "path$p.candidates_tried" "$(sv "$p" m.candidates_tried)"
        [ "$st" = PARTIAL ] && rl_kv "path$p.note" "extra test binaries failed: $(sv "$p" note | cut -c1-100)"
        rl_kv "path$p.rc" 0
        tl=""; for t in t_edge t_nodechk t_xshmem t_pe_geo; do [ -x "$(dirname "$abs")/tests/$t" ] && tl="$tl $t"; done; [ -x "$(dirname "$abs")/../tools/unpack_digits" ] && tl="$tl unpack_digits"
        rl_kv "path$p.tests" "${tl# }"
        wm=$(sv "$p" e.MNRUN_MODULES)
        if [ -z "$wm" ] && [ "$REMOTE_SITE" != aac7 ]; then wm=$(loaded_mods "$p"); [ -n "$wm" ] && rl_kv "path$p.modules_stored" "$wm (candidate 'as is': the loaded modules)"; fi
        [ -z "$wm" ] && [ "$REMOTE_SITE" != aac7 ] && rl_kv "path$p.modules_note" "no module list known: REMOTE_MODULES_P$p not stored (mnrun.sh would use its built-in default)"
        if [ $p = 1 ]; then ok1=1; rl_site_set REMOTE_ECALC1 "$abs"; [ -n "$wm" ] && rl_site_set REMOTE_MODULES_P1 "$wm"; else ok2=1; rl_site_set REMOTE_ECALC2 "$abs"; [ -n "$wm" ] && rl_site_set REMOTE_MODULES_P2 "$wm"; fi
        u=$(sv "$p" e.MNRUN_UNLOAD); [ -n "$u" ] && rl_site_set REMOTE_UNLOAD "$u"
    else
        rl_site_unset "REMOTE_ECALC$p"
        rl_kv "path$p.rc" "$rc"; rl_kv "path$p.result" "FAILED: $(sv "$p" note | cut -c1-200)"
        rl_kv "path$p.log" "$o/raw/build.log"
    fi
done
case $PATHS in
  1) [ $ok1 = 1 ] && v=PASS || v=FAIL;;
  2) [ $ok2 = 1 ] && v=PASS || v=FAIL;;
  both) if [ $ok1 = 1 ] && [ $ok2 = 1 ]; then v=PASS; elif [ $ok1 = 1 ] || [ $ok2 = 1 ]; then v=PARTIAL; else v=FAIL; fi;;
esac
[ $PATHS = both ] && [ $ok1 = 0 ] && [ $ok2 = 1 ] && rl_kv carry_on "path 1 did not build: continue with path 2 (--path 2 in the next stages; the S6 route needs path 1)"
[ $PATHS = both ] && [ $ok1 = 1 ] && [ $ok2 = 0 ] && rl_kv carry_on "path 2 did not build: continue with path 1 (--path 1 in the next stages)"
[ "$v" = FAIL ] && rl_finish FAIL "build failed" "$HERE/show.sh $RUN errors"
rl_finish "$v" "$HERE/s3_gate.sh --nodes 1,2 --path $([ $ok1 = 1 ] && [ $ok2 = 1 ] && echo both || { [ $ok1 = 1 ] && echo 1 || echo 2; }) --digits 1e9"
