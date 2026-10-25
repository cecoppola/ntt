#!/bin/bash
# sys01 - the node part of the probe on one allocated node (probe.sh is maintained by the probe agent and does its own srun --jobid;
# this only calls its interface: probe.sh --only node --jobid J --out DIR).
. "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
tgt_init
P=$TGT_ROOT/probe/probe.sh
[ -f "$P" ] || { tgt_kv status SKIP; tgt_kv note "probe/probe.sh not present"; exit 0; }
bash "$P" --only node --jobid "$JOBID" --out "$OUT" > "$OUT/raw/probe_node.out" 2>&1
rc=$?
[ -f "$OUT/facts.tsv" ] && tgt_kv m.facts "$(wc -l < "$OUT/facts.tsv") lines"
exit $rc
