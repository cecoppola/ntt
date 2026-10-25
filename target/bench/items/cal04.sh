#!/bin/bash
# cal04 - all-to-all bandwidth against message size (TQ73): t_xshmem a2a (slab 64 KiB x 4^k up to 64 MiB) at 8 and 2 nodes, plus stream and lat at 2 nodes.  The sweep starts at 64 KiB (t_xshmem), not 4 KiB.
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
N=8; [ "$(hosts_n)" -ge 8 ] || N=$(hosts_n)
XT_TMO=8 xt_run a2a8 $PN $N a2a XT_MAXMB=64 XT_REPS=3 || note "a2a $N rc $?"; xt_kv a2a8 a2a || true
tleft_ok 0.6 && { XT_TMO=6 xt_run a2a2 $PN 2 "a2a stream lat" XT_MAXMB=64 XT_REPS=3 || note "2-node rc $?"; for t in a2a stream lat; do xt_kv a2a2 $t || true; done; }
tgt_kv q.TQ73 "a2a peak per node $(xt_peak a2a8 a2a) GB/s at $N nodes, $(xt_peak a2a2 a2a) GB/s at 2 nodes (path $PN); curve in m.a2a8.a2a.*"
OPT_NO_RUNS_OK=1; finish
