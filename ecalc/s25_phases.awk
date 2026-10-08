# s25_phases.awk - A37-Q9: by-phase table of ONE run's log (or res extract) from the `bs: level` lines (ECALC_VERBOSE=2) of all nodes in it.
#   awk -f s25_phases.awk <log>         per level: tier, pairs, mean and max (over the nodes) of the level wall, then the batch tier's scatter/ntt/crt/merge means;
#                                       then per tier the sum over levels of the node-mean, and the run's own `bs` summary line (seeds school batch mdev).
/^bs: level +[0-9]+ / { lv = $3 + 0; tier = $4; np[lv] = $5; T[lv] = tier; n[lv]++
  dt = $12 + 0; sd[lv] += dt; if (dt > mx[lv]) mx[lv] = dt
  if ($14 == "(batch") { tb[lv] += $15 + 0; sc[lv] += $17; nt[lv] += $19; cr[lv] += $21; mg[lv] += $23 }
  if (lv > maxlv) maxlv = lv }
/^bs  +[0-9.]+ s +N / && !sumline { sumline = $0 }
END { if (!maxlv) { print "no `bs: level` lines"; exit }
  printf "%-4s %-6s %10s %5s %8s %8s | %8s %8s %8s %8s %8s   (mean over the nodes in the log, s)\n", "lvl", "tier", "pairs", "nodes", "wall", "wall_max", "batch", "scatter", "ntt", "crt", "merge"
  for (l = 1; l <= maxlv; l++) { if (!n[l]) continue; m = n[l]
    printf "%-4d %-6s %10s %5d %8.2f %8.2f | %8.2f %8.2f %8.2f %8.2f %8.2f\n", l, T[l], np[l], m, sd[l] / m, mx[l], tb[l] / m, sc[l] / m, nt[l] / m, cr[l] / m, mg[l] / m
    W[T[l]] += sd[l] / m; B[T[l]] += tb[l] / m; S[T[l]] += sc[l] / m; N[T[l]] += nt[l] / m; C[T[l]] += cr[l] / m; M[T[l]] += mg[l] / m; L[T[l]]++ }
  print "tier totals (sum over levels of the node-mean):"
  for (t in W) printf "  %-6s %2d levels: wall %7.2f s | batch-detail %7.2f = scatter %6.2f + ntt %6.2f + crt %6.2f + merge %6.2f\n", t, L[t], W[t], B[t], S[t], N[t], C[t], M[t]
  if (sumline) print "run summary: " sumline }
