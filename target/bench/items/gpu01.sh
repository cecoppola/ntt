#!/bin/bash
# gpu01 - ROCm 7.14 vs the next older ROCm module (TQ30): rebuild path 1 under the older module on the login node, ecalc 1e10 layout D, ABBA, 1 node.
# The alternative build is a private copy ($TGT_ROOT/build/<sha>-p1-alt); the shared build is never touched.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
_ec_path 1 >/dev/null 2>&1
CUR=$(echo "$EC_MODS" | grep -oE 'rocm/[0-9][0-9.]*' | head -1)
AVAIL=$(bash -lc 'module -t avail rocm 2>&1' | grep -oE '^rocm/[0-9][0-9.]*' | sort -Vu)
ALT=$(echo "$AVAIL" | grep -B1 -x "$CUR" | head -1); [ "$ALT" = "$CUR" ] && ALT=
[ -n "$CUR" ] && [ -n "$ALT" ] || skip "no older ROCm module than ${CUR:-none} (available: $(echo $AVAIL))"
tgt_kv e.rocm_current "$CUR"; tgt_kv e.rocm_alt "$ALT"
B=$TGT_ROOT/build/$TGT_SHA-p1-alt; MODS_ALT=$(echo "$EC_MODS" | sed "s#rocm/[0-9][0-9.]*#$ALT#")
T1=$(date +%s)
if [ ! -x "$B/ecalc/ecalc" ]; then
    mkdir -p "$B"
    tar -C "$(cd "$EC_DIR/.." && pwd)" -cf - --exclude='*.o' --exclude=ecalc/ecalc ecalc tools | tar -C "$B" -xf -
    bash -lc '[ -n "'"$EC_UNLOAD"'" ] && module unload '"$EC_UNLOAD"' >/dev/null 2>&1; module load '"$MODS_ALT"' >/dev/null 2>&1; cd "'"$B"'/ecalc" && make clean >/dev/null 2>&1; make -j16 XSHMEM=ofi ecalc tools' > "$OUT/raw/make_alt.log" 2>&1
fi
tgt_kv m.alt_build_s "$(( $(date +%s) - T1 )) s"
[ -x "$B/ecalc/ecalc" ] || { tgt_kv note "build under $ALT failed (raw/make_alt.log: $(grep -m2 -E 'error' "$OUT/raw/make_alt.log" | tr '\n' ' ' | cut -c1-200))"; tgt_kv status FAIL; exit 1; }
D=$(dig 1e10); i=0
# warm-up (discarded, not in runs.tsv): the first run on this host is cold and would bias the sequential A B B A
ecalc_run A_warm D 1 "$D" || true
for a in A B B A; do
    i=$((i + 1))
    tleft_ok 0.9 || { note "stopped before run $i: time budget"; break; }
    if [ $a = A ]; then ecalc_run "${a}_r$i" D 1 "$D" || true
    else ECALC1=$B/ecalc/ecalc MNRUN_DIR1=$B/ecalc MNRUN_MODULES_P1=$MODS_ALT ecalc_run "${a}_r$i" D 1 "$D" || true; fi
    rec "${a,,}_r$i" "$a" "$i"
done
arms_report A
tgt_kv q.TQ30 "alt $ALT (B) vs current $CUR (A): ${AB_LINE:-no counted run} at $D digits; negative = the older ROCm is faster"
finish
