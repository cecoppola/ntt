# remote/ - run ecalc on the target with a browser agent as the guide

`remote/` lets a Claude web-app browser agent steer an ecalc session on the target machine through you. The agent cannot run
anything: it reads a static GitHub copy of this repository (branch `remote-v11`) and what you paste. You are the link.
Goal of the session (a few hours, one sitting): the most digits of e in the shortest time, with optimized runs.

Files: `AGENT.md` (the agent reads it first), `ROUTE.md` (the stages), `FACTS.md` (machine, paths, settings, known
problems), `MANIFEST.sha256` (copy check), `run/*.sh` (the scripts the agent's command blocks call; `run/arms.sh` runs several arms side by side in one allocation, opt-in).

## Prepare (once, before the session)

1. Download the branch `remote-v11` from GitHub as one archive (`Code` > `Download ZIP` with the branch selected). Put the archive on the target and unpack it
   anywhere on a file system visible to the compute nodes (no git needed). Do not edit the files.
2. On the target: `cd <unpacked dir> && bash remote/run/s0_check.sh` - checks the copy against the manifest and the tools
   (the agent's first command is the same). Fix a missing or changed file by unpacking again.
3. Claude web app: create a Project, connect the GitHub repository, pick branch `remote-v11`; add the folder `remote/` and the
   extra files the agent should read when asked (`ecalc/README.md`, `PLAN.md`, `TESTING.md`,
   `target/RESTART.md` (only sections 3, 7, 8, 9, 11: the rest is for a Claude Code session on the target and does not apply), `target/ladder/`, `target/bench/QUESTIONS.md`). As the Project instructions paste the first
   block of `remote/AGENT.md` (between the two `----` lines), or just tell the agent to read `remote/AGENT.md` first.
4. Check that the target allows a long allocation (the session holds one allocation of up to 576 nodes) and that a
   scratch directory with a few TB is free (the agent will ask where outputs go; default `$HOME/remote_runs`).

## First message to the agent

The branch name is written in this file (lines 4, 12, 16 and the message below), in `AGENT.md` (the first block and the REPORT header and `COPY=` line) and in the repository `README.md` (the `remote/` row): rename in those places only.

> Read remote/AGENT.md, remote/ROUTE.md and remote/FACTS.md on branch remote-v11. The repository copy is unpacked at
> `<path>` on the target. Session length: `<hours>`. Start with stage S0+S1 and give me the first command block.

## Each round trip

1. Copy the agent's command block (one paste, often one line running a stage script or `chain.sh`) into the target terminal.
2. When it ends, copy the block between `=== REMOTE ... SUMMARY ===` and `=== END ... ===` (30-80 lines) back to the
   agent. If the agent asks for an extract, paste its `show.sh` line and copy that output (80 lines at most).
3. If a command runs long, let it run; the agent plans waits so that you do not poll. Never paste secrets.

Expect about 12-20 round trips for the full route, 5-6 for the minimal route (ROUTE.md).

## Bring the results home

At the end the agent writes one REPORT block (format in AGENT.md). Copy it whole, plus `$REMOTE_OUT/site.env` if the
agent asks, into a local Claude Code session on the development machine; that session files the numbers (the REPORT
is written to be pasted as is). Run directories under `$REMOTE_OUT` keep the full logs; remove them only after the
report is home.
