#!/bin/bash
# g2_sum.sh [--wait]: summarise the G2 soak (~/g2/<tag>/runs.tsv) into ~/g2/summary.txt; with --wait first wait for every
# controller marker (~/g2/<tag>/CTL_DONE), then write ~/g2/DONE.  Per node: runs, OK, HANG, SEGV, VFAIL, OTHER, median/max OK wall.
O=$HOME/g2
if [ "$1" = --wait ]; then
  while :; do n=0; for d in $O/*/; do [ -e $d/driver.txt ] && [ ! -e $d/CTL_DONE ] && n=1; done; [ $n = 0 ] && break; sleep 120; done
fi
{ echo "G2 soak summary $(TZ=America/New_York date '+%m-%d %H:%M') ET (4e9 digits, RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30, timeout 3x median)"
  printf '%-5s %5s %5s %5s %5s %5s %5s %8s %8s\n' node runs OK HANG SEGV VFAIL OTHER med_s max_s
  for d in $O/*/; do t=$(basename $d); [ -s $d/runs.tsv ] || continue
    awk -F'\t' -v t=$t '{n++; c[$6]++; if($6=="OK"){w[++k]=$5}} END{asort(w); m=(k? (k%2?w[(k+1)/2]:(w[k/2]+w[k/2+1])/2):0); printf "%-5s %5d %5d %5d %5d %5d %5d %8.1f %8d\n", t,n,c["OK"],c["HANG"],c["SEGV"],c["VFAIL"],c["OTHER"],m,w[k]}' $d/runs.tsv
  done | tee /tmp/g2_rows.$$
  awk '{r+=$2;o+=$3;h+=$4;s+=$5;v+=$6;x+=$7} END{printf "%-5s %5d %5d %5d %5d %5d %5d\n","ALL",r,o,h,s,v,x}' /tmp/g2_rows.$$; rm -f /tmp/g2_rows.$$
  echo "non-OK runs:"; cat $O/*/runs.tsv 2>/dev/null | awk -F'\t' '$6!="OK"' | head -40
  ls -d $O/*/hang/run* 2>/dev/null | head -20
} > $O/summary.txt 2>&1
[ "$1" = --wait ] && touch $O/DONE
