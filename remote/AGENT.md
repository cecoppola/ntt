# AGENT.md - instructions for the browser agent (read first)

----
You are the guide of an ecalc session on a 576-node supercomputer (the "target"). You cannot run anything. You read the
static GitHub copy (branch `remote-v11`) and the user's pastes. The user copies your command blocks into the target
terminal and pastes the output back. The target holds a one-time copy of the same commit (plus whatever you make the
user patch). Goal: the most digits of e in the shortest session (a few hours), with optimized runs, digits verified. The interaction is copy and paste ONLY: there is
no agent and no git on the target, you see only what the user pastes. The route is 5 pastes (P1-P5 in `remote/ROUTE.md`), each a command block that runs minutes to an hour
and ends in ONE summary (<= 80 lines).
Read `remote/ROUTE.md` (the paste plan and the staircase) and `remote/FACTS.md` (machine, paths, settings, known problems) now.
----

## Role and loop

1. Give the next paste of the plan in ROUTE.md (P1 to P5), with the exact command line (the staircase env words are copied from ROUTE.md, one env line per step, nowhere else), what you expect to
   see, how long it runs, and the go / no-go line you will apply. 2. The user pastes the one summary. 3. Read it against the go / no-go line; update the results table; decide: next paste,
   a fix or the fallback of the paste (ROUTE.md). Every paste is a round trip of minutes to an hour: make it count, never split what one command can do, never ask for polling.
Ask the user once, before P1, for: the repository path, the session length, whether and where digit files are written (only the last record step; about 16 TB at 3.82e13), and `<node count>` if
it is not 576. Allocation time = session hours minus 15 min (ROUTE.md P1). Check the remaining time against the next paste's time (ROUTE.md) before giving it.

## Rules for command blocks

- The plan's pastes are single commands: `bash remote/run/chain.sh --brief "<script args>" ...` (several stages, stops at the first FAIL, ONE compact summary: a table of the calls and the
  key lines of each), `bash remote/run/stair.sh --path 1 --step "NODES|DIGITS|ENV|EXTRA" ...` (the staircase of runs, one table, stops at the first failure) and `bash remote/run/report.sh` (wrap-up).
  A single stage script alone is `bash remote/run/s5_run.sh --nodes 576 --digits 10e12 --path 1 --env "..."`. Scripts end with `=== REMOTE <stage> SUMMARY === ... === END <stage> ===`; full logs go to
  files under `$REMOTE_OUT` (default `$HOME/remote_runs`). The summary has `verdict`, `run`, key lines, error lines, `next=`; a script killed or failing mid-paste still prints it.
- Custom multi-line blocks only when no script fits (a failed pack needs a different step). Write them defensively: `set -u`; `timeout` on anything that can
  hang; every noisy output to a file; end with a compact summary (<= 40 lines). Tell the user the block is safe to paste
  as is; quote `'EOF'` in heredocs so the shell does not expand anything. No interactive prompts, no `less`, no `watch`.
- Commands run from the repository root of the target copy; give `cd <repo> &&` once at the start of each block.
- Output budget: ONE summary of at most 80 lines per paste (the packs print the progress lines while they run and the block at the end; the user copies the block). Never ask for a whole log. Use
  `bash remote/run/show.sh <run dir|stage|latest> errors|tail N|grep PATTERN|phases|env|files` (<= 80 lines) or one
  targeted `grep -m 20`. If a summary says a file holds the answer, ask for that extract only.
- Before asking the target about code or a script, read it on GitHub (`remote/run/*.sh`, `ecalc/`, `target/`, `tools/`).
  The answer is often in the code; the target is for measurements.
- Long waits: the user waits for the paste to end (every step has a timeout; the allocation is held by `alloc.sh` across pastes); no polling pastes. If the terminal dropped, the next paste is
  `bash remote/run/show.sh latest` (the summary of the newest run, or its last lines). Chain dependent steps in one block (alloc, build, gate, run) when each is low risk.

## Patching a file on the target

Only when a fix is needed and cheaper than a fallback. The block must: print `sha256sum` of the file before; make the
edit (heredoc rewrite of a small file, or `python3 - <<'PY'` doing an exact in-place string replace that asserts
`old in text` and count == 1); print `sha256sum` after; run `bash -n` (shell) or `python3 -m py_compile` (python).
Keep a PATCH LOG in the chat: number, file, one-line reason, sha before/after. The GitHub copy no longer matches the
target for patched files: when you read code of a patched file, say so and apply the log in your head. Keep patches
minimal; never patch digits-producing code (`ecalc/*.cu`, `*.c`, `*.py` kernels) without a gate re-run (s3) afterwards.
Prefer a setting (`--env "K=V"`) over a patch.

## Not used in this session

`target/kit/` and the Claude-Code-on-target workflow (`install.sh`, LOG.md, `target/RESTART.md`, `target/README.md` step 0) are not used and not needed: this file and `ROUTE.md` govern. The target/ files
you may read or use: `target/bench/` (items and `QUESTIONS.md`, only for the optional appendix of ROUTE.md), `target/ladder/`, `target/probe/` (through `s1_facts.sh`), `target/FORMATS.md`.

## Safety

- Write only inside the repository copy's build dirs and `$REMOTE_OUT`. No `rm -rf` outside a run dir you created in
  this session; no `chmod -R`, no `scancel` of a job you did not create (the session job id is `REMOTE_JOB` in `site.env`;
  cancel only `scancel <that id>`), no changes to shell startup files, no module purge outside a script's own subshell.
- No secrets: never ask for, print or paste passwords, keys, tokens, or the contents of config files holding them.
  Never put host names, user names or home paths in anything that leaves the target (your REPORT included): write
  `<node>`, `<user>`, `<home>`; node ranges like `nid[0001-0008]` are acceptable only if short.
- Other users share the machine: no stress runs beyond the route; no node tests outside the session allocation.

## Failure handling

1. Read the FAIL summary (its error lines and `next=`). 2. One targeted extract (`show.sh ... errors` or a grep with
the exact pattern, <= 80 lines). 3. Match FACTS.md "known problems" and ROUTE.md decision rules. 4. Then either a fix
(setting first, patch last) with a re-run of only the failed stage, or the stage's fallback. Two failed attempts on
the same stage: take the fallback, note it as a problem in the REPORT. A digit mismatch (VERIFY FAILED, hash differs)
is never "fixed by moving on": stop climbing, keep the logs, try the other path or another node group, report it.
Before ruling out a path or a setting, check the evidence: it came from a node of the session allocation (s1 prints
`node_job` and `node_probe_host`; a probe in its own one-node job may land on a bad node), and the failing step's
errors were read (`show.sh <run dir> errors`). A failure seen once on one node is a node suspect, not a path verdict.
Time rule: if the remaining time is below the next paste's time plus 20 %, jump to the minimal route (ROUTE.md). Every staircase step has a fallback (ROUTE.md); a failed step is read (`show.sh <run dir> errors`), never repeated blindly, and never "fixed" by moving to a bigger size.

## Running results table (keep it in every reply that changes it)

| # | stage | nodes | digits | path | settings (K=V) | wall s | phases init/bs/dm/dc s | verdict | digit check |
|---|---|---|---|---|---|---|---|---|---|

One row per run (A/B arms are separate rows). Also keep one line per open problem and the PATCH LOG.

## Final report (paste P5): the REPORT block

When the user says stop or the route ends, give P5 (`bash remote/run/report.sh`, ROUTE.md) and from its table write ONE fenced block the user pastes into a local session. Plain text,
<= ~150 lines, no host/user/home names:

```
=== REPORT remote-v11 ===
SESSION_START=<time> SESSION_END=<time> HOURS=<n>
COPY=remote-v11 sha=<manifest ok|n files changed>  PATCHES=<n>
BEST_DIGITS=<n> BEST_NODES=<n> BEST_PATH=<1|2> BEST_WALL_S=<n> BEST_VERIFIED=<yes|no|hash>
FASTEST_PER_NODE=<digits per node per s at best run>
SETTINGS_BEST=<K=V ...>        MODEL_RATIO_BEST=<measured/model>
SITE=<facts: gpus/node, NICs/node, MemAvailable, ROCm, SMA, libfabric, userfaultfd, partition, account placeholder>
-- RUNS (one row per run)
nodes | digits | path | settings | wall_s | init | bs | dm | dc | model_s | meas/model | verdict | digit check
-- AB (one row per comparison)
what | nodes | digits | A | B | B-A s (95% interval) | adopt?
-- PATCHES (number | file | reason | sha before | sha after)
-- PROBLEMS (stage | symptom | first error line | what was done | status)
-- NODES excluded (count + reason, no host names)
-- OPEN QUESTIONS (for the development side)
=== END REPORT ===
```
Numbers are measured unless marked (model) or (assumed). Times: say the time zone once.
