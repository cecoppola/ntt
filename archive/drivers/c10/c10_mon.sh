#!/bin/bash
# c10_mon.sh LABEL SECONDS : per-node 1 Hz host-memory sampler (self-terminating; no network program, no GPU use).
# Output ~/sess1011c/c10/mon/<LABEL>.<host>.tsv: epoch, MemFree, Cached, Dirty, Writeback, AnonHugePages (kB), memory PSI some/full avg10,
# vmstat deltas that show reclaim/compaction stalls: allocstall_*, pgscan_direct, pgsteal_direct, compact_stall, thp_fault_fallback.
d=$HOME/sess1011c/c10/mon; mkdir -p $d; f=$d/$1.$(hostname -s).tsv; end=$((SECONDS + ${2:-300}))
echo -e "epoch\tMemFree\tCached\tDirty\tWriteback\tAnonHuge\tpsi_some10\tpsi_full10\tallocstall\tpgscan_direct\tpgsteal_direct\tcompact_stall\tthp_fallback" > $f
while [ $SECONDS -lt $end ]; do
  awk -v t=$(date +%s.%N) -v psi="$(awk '{print $2 \" \" $0}' /proc/pressure/memory 2>/dev/null | tr '\n' ' ')" '
    FNR==NR {m[$1]=$2; next} {v[$1]=$2}
    END {split(psi, p, " "); s=""; for (i in p) if (p[i] ~ /^avg10=/) { sub("avg10=","",p[i]); a[++n]=p[i] }
         as=0; for (k in v) if (k ~ /^allocstall/) as+=v[k];
         printf "%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n", t, m["MemFree:"], m["Cached:"], m["Dirty:"], m["Writeback:"], m["AnonHugePages:"], a[1], a[2], as, v["pgscan_direct"], v["pgsteal_direct"], v["compact_stall"], v["thp_fault_fallback"]}' /proc/meminfo /proc/vmstat >> $f
  sleep 1
done
