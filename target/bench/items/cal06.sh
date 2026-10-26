#!/bin/bash
# cal06 - small-message latency against node count (TQ75), 2..64 nodes: t_xshmem lat (ring token hop and pingpong).  No barrier/allreduce program exists in tests/: NOT covered (INIT_SCALE barrier term); needs a small xshmem.h program (follow-up).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
PN=2; ecalc_have 2 || PN=1; need_path $PN
for n in 2 4 8 16 32 64; do
    [ "$(hosts_n)" -ge $n ] || continue; tleft_ok 0.8 || { note "stopped before n=$n"; break; }
    XT_TMO=3 xt_run "lat$n" $PN $n lat XT_MAXMB=1 XT_REPS=3 || note "lat n=$n rc $?"
    xt_kv "lat$n" lat || true
    R="$R n$n:ring $(awk -F'\t' '/ring/ { print $2; exit }' "$OUT/raw/lat$n.lat.tsv" 2>/dev/null)us;"
done
tgt_kv q.TQ75 "ring hop latency (us) by node count:$R; barrier/allreduce not measured (no program)"
OPT_NO_RUNS_OK=1; finish
