# HANG: t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13)

**Verdict: not a hang. The queue items' own `timeout 1500` around `mnaccept.sh` expired while the last unit test was starting or running.**

## Evidence
- ADOPT's two items ran `cd <wt>/ecalc && timeout 1500 ./mnaccept.sh <hold> --only unit,e9` and recorded `mnaccept rc=124` (the timeout
  exit code). Both summaries show the eight single-process unit tests passed and no `t_mn_grid` line: the script was killed before
  it could print one (t_mn_grid is the 9th and last unit step; its own limit is 1200 s, so it could not itself have timed out).
- A repeat on a 2-node hold (measured, current main b80ee2e, defaults batch_pipe/crt_scan/dm_mn_wrap on): `mnaccept.sh --only unit`
  ran all 9 steps, 9 passed, 0 failed, t_mn_grid VERIFY OK (180 checks), in 1491 s total (so `unit` alone is already 99% of a
  1500 s limit; `e9` after it adds ~1 min more). A second repeat was passing the same steps when the item's 50 min cap ended it.
- The per-run logs of the two ADOPT runs were removed with ADOPT's worktree (not recoverable); standalone t_mn_grid is 100-130 s
  (measured, ADOPT A/B: 101-111 s for default, BS_BATCH_PIPE=0, RNS_CRT_SCAN=0, both).
- The earlier "passes" (SIMPM 18/18, SH3 15/15) fit because t_mul 20 etc. vary by run; the margin is small, so the outcome depended on speed.

## Fix / rule
- No product or harness bug found; no stack dump was needed (nothing hung). Queue items running `mnaccept.sh --only unit[,...]`
  must use `timeout >= 2400` (unit ~1500 s measured) and EST_MIN >= 45 (daemon cap is 1.5 x EST_MIN + 5 min); `--only grid` alone
  is ~2 min.
- Optional later: print each unit step's elapsed time in mnaccept.sh so a cut-off run shows where the clock went.

## RESUME
Closed. Raw data: aac7 `~/hang/` (sum_*.txt, run*.out, grid_1.log).
