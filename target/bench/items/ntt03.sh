#!/bin/bash
# ntt03 - A/A spread at 1 node: 8 sequential runs of 1e10 layout D (TQ58); bimodal slow mode check (aac7: about half the runs 5-8 % slower).
. "$(dirname "${BASH_SOURCE[0]}")/_opt.sh"
need_path 1
runseq D 1 "$(dig 1e10)" "A A A A A A A A" || true
spread_kv spread
tgt_kv q.TQ58 "${SPREAD_LINE:-no counted run}"
finish
