#!/bin/bash
# cal02 - 1-node scaling points (TQ71): ecalc layout D at 1e9, 1e10, 3e10, 6.6e10, 1e11 (digits per node; aac7 scale 0.1), phase split per size, init timeline on.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
i=0
for x in 1e9 1e10 3e10 6.6e10 1e11; do
    i=$((i + 1)); D=$(fitd 1 D "$(dig $x)")
    tleft_ok 0.8 || { note "stopped before $x: time budget"; break; }
    WRITE=0 one "S$x" 1 D 1 "$D" ECALC_INIT_TL=1
    LG=$E_LOG   # ecalc_run sanitises the tag; E_LOG is the real log path
    tl_kv "$LG" "tl_s$x" || true
    _cat "$LG" | grep -a -E '^(total|bs|dm|recip|10dP) ' | head -8 > "$OUT/raw/s${x}.phase_lines"
    ROW="$ROW $x:${E_TOTAL:-NA}s"
done
tgt_kv q.TQ71 "total s per 1-node size:$ROW (phase split m.s<size>_r1.*)"
finish
