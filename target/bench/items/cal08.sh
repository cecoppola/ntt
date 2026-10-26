#!/bin/bash
# cal08 - single-thread CPU speed (TQ77): ecalc 1e9 with the digit file written (dc timers), init timeline (seed duration), lscpu, and a fixed single-thread loop timed on a compute node.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
WRITE=1 one A 1 D 1 "$(ecalc_digits 1e9)" ECALC_INIT_TL=1
tl_kv "$OUT/raw/a_r1.log" tl || true
ecalc_srun lscpu 0 1 bash -c 'lscpu | grep -E "Model name|^CPU\(s\)|Thread|MHz|L3" ; python3 - <<PY
import time
t=time.time(); x=0
for i in range(30000000): x+=i&7
print("pyloop_s", round(time.time()-t,3))
PY' || true
tgt_kv e.cpu "$(grep -h -m1 'Model name' "$OUT/raw/lscpu.log" | sed 's/.*: *//')"
tgt_kv m.pyloop_s "$(grep -h -m1 pyloop_s "$OUT/raw/lscpu.log" | awk '{print $2}') s"
tgt_kv q.TQ77 "dc ${E_DC:-NA} s at 1e9 digits; seed thread $(grep -h '^m.tl.seed_dur_s=' "$TGT_SUM_DIR/summary.txt" | cut -d= -f2) (aac7 seed 7.589+0.2681 s per 1e9, DC_FMT1 0.0545 s per 1e9)"
finish
