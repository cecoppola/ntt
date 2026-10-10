# s27_xagg.awk: aggregate the `xstats` lines of one run's log (all PEs).  Per (phase, kind): mean per node of n and GB, and the mean seconds per APU thread
# (the PE's sums / 4 threads, averaged over the PEs) of t_post, t_call, t_wait, roff_wait, sig_wait; GB/s per APU thread = GB / t_post.  Then, per APU index, the layered inter-stage GB/s.
function val(s, key,   p) { if (match(s, key " +[0-9.]+")) { p = substr(s, RSTART, RLENGTH); sub(key " +", "", p); return p + 0 } return 0 }
/^xstats pe [0-9]+ phase / { pe = $3; seenpe[pe] = 1; k = $5 " " $7; sub(":", "", k); key[k] = 1
  N[k] += val($0, " n"); G[k] += val($0, " GB"); TP[k] += val($0, "t_post"); TC[k] += val($0, "t_call"); TW[k] += val($0, "t_wait"); RO[k] += val($0, "roff_wait"); SG[k] += val($0, "sig_wait"); RD[k] += val($0, "rounds") }
/^xstats pe [0-9]+ apu / { seenpe[$3] = 1; a = $5 " " $7; sub(":", "", a); akey[a] = 1; AG[a] += val($0, " GB"); AT[a] += val($0, "t_post"); AN[a]++ }
END { np = 0; for (p in seenpe) np++; if (!np) { print "no xstats lines"; exit }
  printf "xstats aggregate over %d PEs (per node: n, rounds, GB; per APU thread: seconds, GB/s = GB per thread / t_post)\n", np
  printf "%-14s %9s %8s %9s %9s %9s %9s %9s %9s %7s\n", "phase kind", "n/node", "rnd/node", "GB/node", "t_post s", "t_call s", "t_wait s", "roff s", "sig s", "GB/s"
  for (k in key) printf "%-14s %9.0f %8.0f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %7.2f\n", k, N[k] / np, RD[k] / np, G[k] / np, TP[k] / np / 4, TC[k] / np / 4, TW[k] / np / 4, RO[k] / np / 4, SG[k] / np / 4, (TP[k] > 0 ? G[k] / TP[k] : 0)
  for (a in akey) printf "apu %-12s mean over %d PEs: GB %.1f, t_post %.1f s, %.2f GB/s\n", a, AN[a], AG[a] / AN[a], AT[a] / AN[a], (AT[a] > 0 ? AG[a] / AT[a] : 0) }
