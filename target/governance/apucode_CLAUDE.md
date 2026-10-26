# CLAUDE.md — rules for ecalc (/home/machinus/apucode; private, outside the repository)

Repository `/home/machinus/apucode/ntt` (`main`; GitHub `main` is authoritative). Read the private
`/home/machinus/apucode/ntt/docs/AGENT_PROTOCOL.md` first: it maps the central docs (README, ALGORITHM, TESTING, PLAN,
TASKS, RESULTS, ecalc/README), the cluster how-to and the task brief. History lives in the local archive
`/home/machinus/apucode/ntt-archive/` (cited as `archive:<path>`). The user sets priorities and security.

## Security (user order 2026-10-12)
- Nothing identifying or secret reaches GitHub: no passwords, keys, tokens, URLs, e-mails, user@host, IPs, full domain
  or host names, user names, home paths (`~/…`), and nothing from `docs/` — in files, names or commit messages.
  Short cluster and node nicknames ("aac7", "aac7-n01") are allowed (D28), and so are public documentation links. Collect commands are written `ssh <cluster> '…'`.
- The cluster password stays in `~/.config/ecalc/aacpw`: use it only through `sshpass -f` (or the toolkit
  `/home/machinus/apucode/bin/`); never read, print or copy it. Never bypass or edit the leak-guard hooks; if one
  blocks you, remove the data.

## Work
- Correctness first: new behaviour behind a switch, off by default; digits identical (gates per TESTING.md, 1 node
  and 2 nodes for multi-node code) before any merge. The user adopts changes on measured data; label every number
  measured / modelled / assumed; times in Eastern.
- Docs are maintained with the code: each fact lives in the one central doc that owns it, updated in the same merge;
  `tools/doccheck.py` (run by the gate) must pass.
- Clusters: aac7 + local only (aac6 set aside). Node runs go through the aac7 queue (`qadd`); no fixed limits on queue use
  (D37 (c), user 2026-10-17: the orchestrating agent decides). Packing (user order 2026-10-17, standing until the
  per-user node limit changes from 6): always run as many items at once as fit, 2-6 concurrent items, never one
  6-node item when smaller items can answer the question. Shapes: 4+2; 2+2+2; 4+1+1; 2+2+1+1; 2+1+1+1+1;
  1+1+1+1+1+1. Size digits per node count; rotate arms across slots; run an A/A control; use 6 nodes only for
  final confirmations against 6-node baselines. Keep the queue stocked so every free node has a runnable item. Never touch other users' jobs or files;
  kill only by single PID; use only your own dirs, worktree and jobs; long waits = a driver with a done-marker.
- Git: work in a worktree `../ntt-wt-<branch>`, keep `ntt/` clean on `main`; commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; sessions end with GitHub, local and aac7 `~/ntt-acc` on
  the same commit.

## Coordinator
- The user talks only to the coordinator: answer their messages first, in plain language; 2–4 lines per agent result;
  short done / running / next at milestones.
- Priority classes (user 2026-10-12), highest first; higher classes get aac7 nodes and agents first:
  1. Fix bugs, correct errors, repair anything broken.
  2. Shrink the active material: remove unused code and files; consolidate sources, scripts and makefiles.
  3. Simplify: algorithms, the parameter and argument systems, how we build, run and test; prefer smaller solutions.
  4. Significant performance: wall-clock time, memory, concurrency, major algorithmic improvements, experiments.
  5. Documentation: keep code and MD files coherent; sync, revise, delete obsolete information.
- The user directs the work in conversation. Execute what they direct at full speed: split it by file ownership into
  parallel agents, do everything possible locally, and use aac7 as the orchestrator judges best.
- Idle capacity is a problem to raise, not to fix silently: whenever an open TASKS item has work that an idle resource
  (local agents, free aac7 nodes) could be doing, tell the user in the next reply and ask whether to start it.
  New work found along the way goes to TASKS "Proposed" for the user.
- Decisions: agents never ask the user. The coordinator keeps `docs/DECISIONS.md`, posts open items as one numbered
  digest at milestones and on request, and closes an item only once its answer is applied where it acts. Live state
  is in `docs/STATUS.md`; `bin/state` prints a one-page check.

## Subagents
- Return once: From/task; Result SUCCESS/FAILED/BLOCKED (+ doubt); Files; Numbers (labelled); Decisions; Follow-ups.

## Compaction (user 2026-10-18)
- The main session writes `ntt/docs/RESTART.md` at ~370k context and keeps working; auto-compaction fires at ~400k;
  after it, a hook prints the restart note and a transcript extract (agent/job IDs, the user's last messages).
- Summary guidance: the compaction summary must cover everything after the restart note was written: new results
  (with numbers and labels), aac7 items started, finished or queued (names, rc), agents launched or returned (IDs), the
  user's messages and decisions verbatim, and the next step in progress. Prefer exact IDs, paths and times over prose.
