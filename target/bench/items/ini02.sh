#!/bin/bash
# ini02 - init timeline at 1 node (TQ34 TQ35): ECALC_INIT_TL=1 at 1e10 and at the large per-node size (6.6e10); seed end, mapping rate, A37-R1 verdict.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
DS=$(dig 1e10); DL=$(fitd 1 D "$(dig 6.6e10)")
ecalc_run small D 1 "$DS" ECALC_INIT_TL=1 || true; rec small S 1; tl_kv "$OUT/raw/small.log" tl_small
tleft_ok 0.7 && { ecalc_run large D 1 "$DL" ECALC_INIT_TL=1 || true; rec large L 1; tl_kv "$OUT/raw/large.log" tl_large; }
g() { grep -h "^m.tl_large.$1=" "$TGT_SUM_DIR/summary.txt" | tail -1 | cut -d= -f2; }
tgt_kv q.TQ34 "seed ends at $(g seed_end_t_s) at $DL digits (gate 18 s; A37-R1 verdict $(g a37r1_verdict))"
tgt_kv q.TQ35 "foreground arena map $(g arena_fg_s_per_gb_apu) per APU thread (aac7 0.070, target-m 0.010 s/GB)"
finish
