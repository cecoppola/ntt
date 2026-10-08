# pairstats.awk: input pairs.tsv (tag nn arm rc wall verify total_line end nodes); output per (node count, arm): runs, rc139, other failures, clean, mean wall / total of clean runs
BEGIN { FS="\t" }
{ key=$2 "n/" $3; n[key]++; node_runs[key]+=$2
  if ($4==139) c139[key]++; else if ($4!=0 || $6!="VERIFY OK") oth[key]++
  else { ok[key]++; w[key]+=$5; if (match($7, /total +[0-9.]+/)) { t[key]+=substr($7,RSTART+6,RLENGTH-6)+0; nt[key]++ } } }
END { for (k in n) printf "%s: runs %d (node-runs %d), rc139 %d, other failures %d, clean %d, mean wall %.1f s, mean total %.1f s\n", k, n[k], node_runs[k], c139[k], oth[k], ok[k], ok[k] ? w[k]/ok[k] : 0, nt[k] ? t[k]/nt[k] : 0 }
