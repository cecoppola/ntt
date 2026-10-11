# s34_stats.awk: pairs.tsv (tag nn arm rc wall verify total end nodes segv) -> per (node count, arm): runs, segv (text count), rc!=0, clean, mean wall
BEGIN { FS="\t" }
{ k=$2 "n/" $3; n[k]++; if ($10>0) sg[k]++; if ($4!=0 || $6!="VERIFY OK") bad[k]++; else { ok[k]++; w[k]+=$5 } }
END { for (k in n) printf "%s: runs %d, segfault %d, rc!=0 or no VERIFY OK %d, clean %d, mean wall %.1f s\n", k, n[k], sg[k], bad[k], ok[k], ok[k] ? w[k]/ok[k] : 0 }
