#!/bin/bash
# aac7q queue item (H5, results/H5.md): paired wall-clock comparison of the decimal (LIMB_BASE=10) and binary (LIMB_BASE=2)
#   limb pipelines, same build (~/ntt-wt/h4main, main before the binary removal), same node, 1 node, no digit writes
#   in the timed runs (VERIFY checks only). ABBA order (odd pairs decimal first, even pairs binary first).
#   1e9: >= 4 pairs, then stop when p < 0.05 or the 95 % CI half-width <= 0.5 s, cap 8. Then one untimed digit-writing run
#   per base with digcmp against the 1e9 reference. Then 1e10 if >= 12 min of budget remain: >= 3 pairs, cap 5, until the budget ends.
#   diff = binary - decimal.  Output under ~/h5/: log/, pairs_1e9.tsv, pairs_1e10.tsv, summary.txt, done-marker H5_DONE.
# OWNER=h5
# NODES=2
# EST_MIN=35
# APPROVED=yes (coordinator, 2026-10-12)
# EXCLUSIVE=0
set -u
J=${SLURM_JOB_ID:-}; NN=${AQ_NODES:-1}; MAIN=${MAIN:-$HOME/ntt-wt/h4main}; OUT=${H5_OUT:-$HOME/h5}
BUDGET_S=${BUDGET_S:-1980}; REF=${REF:-$HOME/ref/e_1000000000.txt}; STUB=${AQ_STUB:-0}
mkdir -p $OUT/log; rm -f $OUT/H5_DONE; SUM=$OUT/summary.txt; : > $SUM; T0=$(date +%s); NPASS=0; NFAIL=0
say() { echo "$(TZ=America/New_York date '+%m-%d %H:%M:%S ET') $*" | tee -a $SUM; }
pass() { NPASS=$((NPASS + 1)); say "PASS $1: $2"; }
fail() { NFAIL=$((NFAIL + 1)); say "FAIL $1: $2"; }
left() { echo $(( BUDGET_S - ($(date +%s) - T0) )); }
finish() { squeue -s -j $J -h 2>/dev/null | grep -v '\.batch\|\.extern' | head -3 | sed 's/^/step still listed: /' | tee -a $SUM
  say "done: $NPASS PASS, $NFAIL FAIL"; echo "$([ $NFAIL = 0 ] && echo SUCCESS || echo FAILED) pass=$NPASS fail=$NFAIL $(date +%s)" > $OUT/H5_DONE; }
trap 'say "item interrupted"; finish; exit 1' INT TERM
if [ "$STUB" != 1 ]; then [ -n "$J" ] && [ "$(squeue -j $J -h -o %T)" = RUNNING ] || { echo "hold $J not running"; exit 2; }; fi
say "H5 decimal vs binary limbs: hold $J, $NN nodes; build main $(git -C $MAIN log --oneline -1 | cut -c1-50)"
cd $MAIN/ecalc || exit 2; source aac7env.sh > /dev/null 2>&1
[ "$STUB" = 1 ] || ldd $MAIN/ecalc/ecalc | grep -q libsma || fail "build" "binary without SHMEM"
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ $MAIN/ecalc/e16_headline.sh)
# run1 <digits> <base> <label> [outfile] -> $TOT (total seconds or NA), $V (ok / FAILED / none)
run1() {
  local dig=$1 base=$2 log=$OUT/log/$3.log of=${4:-}
  if [ "$STUB" = 1 ]; then # stub: fake times, decimal faster
    sleep 1; if [ "$base" = 10 ]; then echo "total   $(awk -v r=$RANDOM 'BEGIN{printf "%.2f", 11+r/32768}') s" > $log; else echo "total   $(awk -v r=$RANDOM 'BEGIN{printf "%.2f", 20+r/32768}') s" > $log; fi
    echo "VERIFY OK" >> $log
  else
    ( cd $MAIN/ecalc && SLURM_JOB_ID=$J MNRUN_NODES=1 MNRUN_LABEL=1 COMM_PORT=$((24000 + RANDOM % 9000)) timeout $((dig > 2000000000 ? 900 : 300)) ./mnrun.sh 1 env $LINE LIMB_BASE=$base ECALC_SEGV_TRACE=1 ./ecalc $dig $of ) > $log 2>&1 < /dev/null
  fi
  TOT=$(sed -E 's/^ *[0-9]+: //' $log | grep -a -m1 '^total' | awk '{print ($2 ~ /^[0-9.]+$/) ? $2 : "NA"}'); TOT=${TOT:-NA}
  V=none; grep -aq 'VERIFY OK' $log && V=ok; grep -aq 'VERIFY FAILED' $log && V=FAILED; }
# stats <tsv> <size> -> prints the summary line(s); sets STOP=1 when the 1e9 stop rule is met (>= 4 pairs, p < 0.05 or CI half-width <= 0.5 s)
stats() {
  awk -F'\t' -v size=$2 -v rule=$3 '
  function lg(x,  c,s,j,y,t){ c[1]=76.18009172947146; c[2]=-86.50532032941677; c[3]=24.01409824083091; c[4]=-1.231739572450155; c[5]=0.001208650973866179; c[6]=-0.5395239384953e-5
    y=x; t=x+5.5; t-=(x+0.5)*log(t); s=1.000000000190015; for(j=1;j<=6;j++){y++; s+=c[j]/y}; return -t+log(2.5066282746310005*s/x) }
  function bcf(a,b,x,  m,m2,aa,c,d,del,h,qab,qap,qam){ qab=a+b; qap=a+1; qam=a-1; c=1; d=1-qab*x/qap; if(d<1e-30&&d>-1e-30)d=1e-30; d=1/d; h=d
    for(m=1;m<=200;m++){ m2=2*m; aa=m*(b-m)*x/((qam+m2)*(a+m2)); d=1+aa*d; if(d<1e-30&&d>-1e-30)d=1e-30; c=1+aa/c; if(c<1e-30&&c>-1e-30)c=1e-30; d=1/d; h*=d*c
      aa=-(a+m)*(qab+m)*x/((a+m2)*(qap+m2)); d=1+aa*d; if(d<1e-30&&d>-1e-30)d=1e-30; c=1+aa/c; if(c<1e-30&&c>-1e-30)c=1e-30; d=1/d; del=d*c; h*=del; if(del-1<1e-12&&1-del<1e-12)break }
    return h }
  function ibeta(a,b,x,  bt){ if(x<=0)return 0; if(x>=1)return 1; bt=exp(lg(a+b)-lg(a)-lg(b)+a*log(x)+b*log(1-x)); if(x<(a+1)/(a+b+2))return bt*bcf(a,b,x)/a; return 1-bt*bcf(b,a,1-x)/b }
  function tcrit(df){ return (df==1)?12.706:(df==2)?4.303:(df==3)?3.182:(df==4)?2.776:(df==5)?2.571:(df==6)?2.447:(df==7)?2.365:(df==8)?2.306:2.262 }
  function tp(t,df){ return ibeta(df/2,0.5,df/(df+t*t)) }
  NR>1 && $5!="NA" { n++; d[n]=$5; A+=$3; B+=$4; if(n==1){a1=$3;b1=$4;d1=$5} s+=$5; ss+=$5*$5 }
  END { if(n<2){ print size ": fewer than 2 complete pairs"; print "STOP=0"; exit }
    m=s/n; sd=sqrt((ss-n*m*m)/(n-1)); se=sd/sqrt(n); tc=tcrit(n-1); t=(se>0)?m/se:0; at=(t<0)?-t:t; p=(se>0)?tp(at,n-1):1; hw=tc*se
    printf "%s: %d pairs: decimal mean %.2f s, binary mean %.2f s, diff binary-decimal %+.2f s, 95%% CI [%+.2f, %+.2f] (half-width %.2f), t %.2f, p %.3g, ratio binary/decimal %.3f, sd of diffs %.2f\n", size, n, A/n, B/n, m, m-hw, m+hw, hw, t, p, (A>0)?B/A:0, sd
    printf "%s: first pair (cold start): decimal %.2f s, binary %.2f s, diff %+.2f s\n", size, a1, b1, d1
    if(n>=3){ m2=(s-d1)/(n-1); s2=(ss-d1*d1); sd2=sqrt((s2-(n-1)*m2*m2)/(n-2)); se2=sd2/sqrt(n-1); hw2=tcrit(n-2)*se2; t2=(se2>0)?m2/se2:0; at2=(t2<0)?-t2:t2; p2=(se2>0)?tp(at2,n-2):1
      printf "%s: without pair 1: %d pairs, diff %+.2f s, 95%% CI [%+.2f, %+.2f], p %.3g\n", size, n-1, m2, m2-hw2, m2+hw2, p2 }
    print "STOP=" ((rule==1 && n>=4 && (p<0.05 || hw<=0.5)) ? 1 : 0) }' $1; }
# pairs <digits> <size> <tsv> <minpairs> <cap> <rule> <needsec>: ABBA loop; each pair starts only when the budget leaves needsec plus the longest pair seen
pairs() {
  local dig=$1 size=$2 tsv=$3 minp=$4 cap=$5 rule=$6 need=$7 p a b va vb order d maxpair=0 t1 t2
  printf 'pair\torder\tdecimal_s\tbinary_s\tdiff_s\tverify\n' > $tsv
  for p in $(seq 1 $cap); do
    [ $(left) -gt $((need + maxpair)) ] || { say "$size: budget, stop before pair $p"; break; }
    t1=$(date +%s)
    if [ $((p % 2)) = 1 ]; then order=AB; run1 $dig 10 ${size}_dec_$p; a=$TOT; va=$V; run1 $dig 2 ${size}_bin_$p; b=$TOT; vb=$V
    else order=BA; run1 $dig 2 ${size}_bin_$p; b=$TOT; vb=$V; run1 $dig 10 ${size}_dec_$p; a=$TOT; va=$V; fi
    t2=$(( $(date +%s) - t1 )); [ $t2 -gt $maxpair ] && maxpair=$t2
    d=$(awk -v a=$a -v b=$b 'BEGIN { if (a == "NA" || b == "NA") print "NA"; else printf "%.2f", b - a }')
    printf '%s\t%s\t%s\t%s\t%s\t%s/%s\n' $p $order $a $b $d $va $vb >> $tsv
    say "$size pair $p ($order): decimal $a s (verify $va), binary $b s (verify $vb), diff $d s, pair wall $t2 s"
    if [ $p -ge $minp ] && [ $p -lt $cap ]; then eval "$(stats $tsv $size $rule | grep '^STOP=')"; [ "${STOP:-0}" = 1 ] && { say "$size: stop rule met after pair $p"; break; }; fi
  done
  stats $tsv $size 0 | grep -v '^STOP=' | tee -a $SUM
  local nv nc; nv=$(awk -F'\t' 'NR > 1 && $6 != "ok/ok"' $tsv | wc -l); nc=$(awk -F'\t' 'NR > 1 && $5 != "NA"' $tsv | wc -l)
  if [ $nv -eq 0 ] && [ $nc -ge 2 ]; then pass "$size pairs" "$nc complete pairs, every run VERIFY OK"; else fail "$size pairs" "$nc complete pairs, $nv with a run not VERIFY OK"; fi; }
# ---- 1e9
pairs 1000000000 1e9 $OUT/pairs_1e9.tsv 4 8 1 120
# ---- digits-identical check, one untimed run per base at 1e9 (writes the digits; compared by digcmp, then deleted)
if [ "$STUB" = 1 ]; then say "stub: digit check skipped"
elif [ -f $REF ] && [ $(left) -gt 600 ]; then
  for b in 10 2; do
    rm -rf $OUT/dig_$b.out*; run1 1000000000 $b dig_1e9_$b $OUT/dig_$b.out
    r=$(bash $MAIN/ecalc/digcmp.sh $OUT/dig_$b.out $REF 2>&1 | tail -1)
    if [ "$r" = identical ] && [ "$V" = ok ]; then pass "digits 1e9 LIMB_BASE=$b" "identical to the reference, VERIFY OK"; else fail "digits 1e9 LIMB_BASE=$b" "digcmp '$r', verify $V"; fi
    rm -rf $OUT/dig_$b.out*
  done
else say "digit check skipped (reference $REF missing or budget)"; fi
# ---- 1e10 (no reference file: VERIFY OK only; the two bases' digits are checked against each other at 1e9 via the reference)
if [ $(left) -ge 720 ]; then pairs 10000000000 1e10 $OUT/pairs_1e10.tsv 3 5 0 60
else say "1e10: skipped, $(left) s of budget left (< 720 s)"; fi
finish
