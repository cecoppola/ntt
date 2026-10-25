#!/bin/bash
# scl03 - exposed division/reciprocal wait (gate S-2, TQ52) and the A37-R1 seed gate at 8 nodes: MN_WAIT_STATS=1 ECALC_INIT_TL=1, path 2, 4e9 digits per node.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 2
N=8; D=$(fitd $N C "$(dig 4e9 $N)")
one S 1 C $N "$D" MN_WAIT_STATS=1 ECALC_INIT_TL=1
tl_kv "$OUT/raw/s_r1.log" tl || true
wait_kv "$OUT/raw/s_r1.log" wait 4 || true
g() { grep -h "^m.$1=" "$TGT_SUM_DIR/summary.txt" | tail -1 | cut -d= -f2; }
tgt_kv q.TQ52 "exposed dm wait $(g wait.dm_wait_max_s) per thread (reciprocal $(g wait.recip_wait_s), division $(g wait.div_wait_max_s)); gate 30 s: $(g wait.s2_verdict); seed gate $(g tl.a37r1_verdict); $N nodes, $D digits"
finish
