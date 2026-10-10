# s35_stats.awk: pairs.tsv (tag nn arm rc wall verify total end nodes segv k ecalc_total) -> per arm: runs, segfaults (text count), other
# failures (rc != 0 or no VERIFY OK, no segfault text), clean, mean wall (driver, 2 s resolution) and mean ecalc 'total'; then the paired
# difference C - A over consecutive non-B runs of the same worker (both clean), mean with a 95 % CI (Student t), wall and ecalc total.
BEGIN { FS = "\t" }
function tq(df,  z) { z = 1.959964; return z + (z^3 + z) / (4 * df) + (5 * z^5 + 16 * z^3 + 3 * z) / (96 * df^2) }   # t_0.975 (Cornish-Fisher)
function ci(name, n, s, ss,  m, sd, h) {
  if (n < 2) { printf "%s: %d pairs (too few for a CI)\n", name, n; return }
  m = s / n; sd = sqrt((ss - n * m * m) / (n - 1)); h = tq(n - 1) * sd / sqrt(n)
  printf "%s: %d pairs, mean %+.2f s, 95%% CI [%+.2f, %+.2f] s, sd %.2f s\n", name, n, m, m - h, m + h, sd }
{ a = $3; n[a]++; clean = ($4 == 0 && $6 == "VERIFY OK")
  if ($10 > 0) sg[a]++; else if (!clean) oth[a]++
  if (clean) { ok[a]++; w[a] += $5; if ($12 != "NA" && $12 != "") { et[a] += $12; ne[a]++ } }
  if (a == "B") next
  if (pend[$1] == "" || pend[$1] == a) { pend[$1] = a; pw[$1] = $5; pe[$1] = $12; pc[$1] = clean; next }
  if (pc[$1] && clean) {
    if (a == "C") { dw = $5 - pw[$1]; de = $12 - pe[$1] } else { dw = pw[$1] - $5; de = pe[$1] - $12 }
    np++; sw += dw; sww += dw * dw
    if ($12 != "NA" && pe[$1] != "NA") { ne2++; se += de; see += de * de } }
  pend[$1] = "" }
END {
  split("A C B", ord, " "); for (i = 1; i <= 3; i++) { a = ord[i]; if (a in n) printf "arm %s: runs %d, segfault %d, other failures %d, clean %d, mean wall %.1f s, mean ecalc total %.1f s\n", a, n[a], sg[a] + 0, oth[a] + 0, ok[a] + 0, ok[a] ? w[a] / ok[a] : 0, ne[a] ? et[a] / ne[a] : 0 }
  ci("paired C-A wall (driver)", np, sw, sww)
  ci("paired C-A ecalc total", ne2, se, see) }
