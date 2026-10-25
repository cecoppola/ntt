#!/bin/bash
# ini05 (TQ23) - init timeline on 8 nodes: ecalc 1e10 (layout P, path 1; path 2 when path 1 is absent) with ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2
# MNRUN_LABEL=1, then tools/initchk.py (rc 0 ok, 3 outlier flagged, 2 cannot parse) = the first-touch stall / slow-node check.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
S=P; PN=1; ecalc_have 1 || { S=C; PN=2; }
ecalc_have $PN || { tgt_kv note "no binary"; ecalc_end SKIP; }
ecalc_run tl $S 8 "$(ecalc_digits 1e10)" ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 MNRUN_LABEL=1; ecalc_kv tl
IC=$(ecalc_tool $PN tools/initchk.py)
if [ -f "$IC" ]; then
    LG=$E_LOG; [ -f "$LG" ] || LG=$LG.gz
    zcat -f "$LG" > raw/tl_plain.log 2>/dev/null
    python3 -I "$IC" raw/tl_plain.log > raw/initchk.out 2>&1; IR=$?
    rm -f raw/tl_plain.log
else IR=NA; echo "no initchk.py" > raw/initchk.out; fi
tgt_kv m.initchk_rc "$IR"; tgt_kv e.initchk_head "$(head -3 raw/initchk.out | tr '\n' '/' | cut -c1-240)"
case $IR in 0) V="no outlier";; 3) V="OUTLIER flagged";; 2) V="timeline not parsable";; *) V="initchk not run";; esac
ecalc_q TQ23 "8 nodes 1e10 ($S): $(ecalc_brief tl); init timeline: $V (initchk rc $IR); $(ecalc_phases)"
ecalc_ok || ecalc_end FAIL
[ "$IR" = 0 ] && ecalc_end PASS || ecalc_end PARTIAL
