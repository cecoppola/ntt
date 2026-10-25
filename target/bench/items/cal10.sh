#!/bin/bash
# cal10 - memory per process (TQ79): planner fit and measured device/host peaks at 1 node for layout D, path 1 shape P and path 2 shape C at the same size (memory-table device column and VmHWM per process, MEM_REPORT_DEVS=1).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
D=$(fitd 1 P "$(dig 5e10)")
for s in D P C; do
    [ "$s" = C ] && ! ecalc_have 2 && continue
    ecalc_plan 1 "$D" $s; tgt_kv m.plan_$s.dev_gb "$P_DEV_GB GB"; tgt_kv m.plan_$s.node_gb "$P_NODE_GB GB"; tgt_kv e.plan_$s.fit "$P_FIT"
    tleft_ok 0.8 || break
    one "$s" 1 $s 1 "$D"
done
tgt_kv q.TQ79 "peak host GB per node D/P/C: $(for s in d p c; do grep -h "^m.${s}_r1.hostpeak_gb=" "$TGT_SUM_DIR/summary.txt" | cut -d= -f2; done | tr '\n' ' ') (planner in m.plan_*.node_gb); device peaks in m.*_r1.devpeak_gb; $D digits, 1 node"
finish
