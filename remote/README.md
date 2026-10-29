# remote/ - run ecalc on the target with a browser agent as the guide

`remote/` lets a Claude web-app browser agent steer an ecalc session on the target machine through you. The agent cannot run
anything: it reads a static GitHub copy of this repository (branch `remote-v12`) and what you paste. You are the link.
Goal of the session (a few hours, one sitting): the most digits of e in the shortest time, with optimized runs. The interaction is copy and paste only, in
**5 command pastes** for the whole route (ROUTE.md: check + build + gates; screen + 576 nodes; the first record run; the staircase of larger runs; report), each one a
single command block that runs for minutes to an hour and ends in ONE summary of at most 80 lines that you paste back.

Files: `AGENT.md` (the agent reads it first), `ROUTE.md` (the stages), `FACTS.md` (machine, paths, settings, known
problems), `MANIFEST.sha256` (copy check), `run/*.sh` (the scripts the agent's command blocks call: `chain.sh --brief` runs several stages in one paste, `stair.sh` runs the staircase of record sizes and stops at the first failure, `report.sh` is the wrap-up (it also runs `collect.sh`, the bundle of all logs); `run/arms.sh` runs several arms side by side in one allocation, opt-in).

## Prepare (once, before the session)

1. Download the branch `remote-v12` from GitHub as one archive (`Code` > `Download ZIP` with the branch selected). Put the archive on the target and unpack it
   anywhere on a file system visible to the compute nodes (no git needed). Do not edit the files.
2. On the target: `cd <unpacked dir> && bash remote/run/s0_check.sh` - checks the copy against the manifest and the tools
   (the agent's first command is the same). Fix a missing or changed file by unpacking again.
3. Claude web app: create a Project, connect the GitHub repository, pick branch `remote-v12`; add the folder `remote/` and the
   extra files the agent should read when asked (`ecalc/README.md`, `target/ladder/`, `target/bench/QUESTIONS.md`). As the Project instructions paste the first
   block of `remote/AGENT.md` (between the two `----` lines), or just tell the agent to read `remote/AGENT.md` first.
4. Check that the target allows a long allocation (the session holds one allocation of up to 576 nodes) and that a
   scratch directory with a few TB is free (the agent will ask where outputs go; default `$HOME/remote_runs`). Digit files are written only on the last record step and only if
   you say so: that is 0.41667 bytes per digit (22.5 TB at 5.4e13, 25.0 TB at 6e13; the agent asks once, with the directory).
5. Start the terminal inside `tmux` or `screen` if the target has one (a dropped connection then does not kill a run in progress); otherwise stay connected during a paste.

## First message to the agent

The branch name is written in this file (lines 4, 12, 16 and the message below), in `AGENT.md` (the first block and the REPORT header and `COPY=` line) and in the repository `README.md` (the `remote/` row): rename in those places only.

> Read remote/AGENT.md, remote/ROUTE.md and remote/FACTS.md on branch remote-v12. The repository copy is unpacked at
> `<path>` on the target. Session length: `<hours>`. Digit files: `<no | only the last step, in <dir>>`. Give me paste P1.

## Each paste

1. Copy the agent's command block (one command: `chain.sh --brief ...`, `stair.sh ...` or `report.sh`) into the target terminal and let it run to the end: nothing needs polling, every step has a
   timeout, and a failed step still prints the summary.
2. When it ends, copy the block between `=== REMOTE ... SUMMARY ===` and `=== END ... ===` (at most 80 lines; for a `chain.sh --brief` or `stair.sh` paste: the one block at the end) back to the agent.
   If the agent asks for an extract, paste its `show.sh` line and copy that output (80 lines at most).
3. The agent reads the go / no-go line of the paste against the summary and gives the next paste, a fix or a fallback. Never paste secrets.

Five pastes for the whole route when nothing fails (P1 check, build, gates; P2 screen and 576 nodes at 4.608e12; P3 first record run; P4 staircase; P5 report); expect 6-8 with fallbacks. About 3 h
without queue waits (ROUTE.md has the times). Time short: ROUTE.md "Minimal route".

## Logs for offline analysis

Record runs are launched with the exhaustive log on, because logs cannot be added afterwards: `export REMOTE_FULLLOG=1` before `stair.sh` (or `--fulllog` on `run/s5_run.sh`; off by default). It adds the
digit-neutral diagnostic words `MN_WAIT_STATS=1 ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=1` (one line per node each), per run the `capture/` files (launch line, environment without secrets, modules, `scontrol show job`, host list,
binary sha256), a node snapshot before and after (`snap/`: meminfo, numastat, amd-smi, every nonzero Slingshot NIC counter; `snap/nic_summary.csv` = traffic after minus before per node and NIC, bytes estimated from the NIC's packet size
buckets) and a sampler every 30 s per node (`monitor/<host>.csv`: MemAvailable, GPU power / temperature / memory, NIC traffic). Left out because they synchronise the hot path or print per PE and period:
`XSHMEM_ROUND_STATS`, `DIST_STATS`, `DB_POOL_VERBOSE`, `ECALC_MEM_SAMPLE`.

Nothing can be copied off the target: only what is printed to the screen leaves it. `bash remote/run/collect.sh` freezes every run directory into one directory on the target (`$REMOTE_OUT/collect_<time>/`, categories:
`summaries`, `stats` = the diagnostic and phase / memory lines of the rank logs, `nodes` = per-node NIC roll-up and the cross-node timeline, `env` = capture files, versions, Slurm accounting (`sacct -j <job> --format=ALL -P`), `nodes_full`, `raw` = the
rest of the rank logs; digit files are not in it, only their path, size and hash). `collect.sh --plan` prints the inventory and the number of parts (pastes) a full print takes; `collect.sh --print [--part K] [--only CATEGORY] [--lines N] [--compact]` prints
the parts, each framed `=== ECALC LOG PART K/N <category> ... sha256=... ===` ... `=== END PART K/N ===` (same text every time, so a lost paste is reprinted with `--part K`; `--compact` prints gzip+base64 of the same content, about 5-7 times smaller; the decode recipe is in part 1);
`--only raw --ranks first,last,busiest,slowest [--grep REGEX]` prints a filtered raw set (the plan shows its part count; lines that name no rank are kept); `--paste` prints a 60-line digest. Print the categories in order and stop when enough is home: the first four are small. `report.sh` runs the freeze and prints the plan before it releases the allocation (`--no-collect` skips).

## Bring the results home

At the end the agent writes one REPORT block (format in AGENT.md). Copy it whole, plus `$REMOTE_OUT/site.env` if the
agent asks, into a local Claude Code session on the development machine; that session files the numbers (the REPORT
is written to be pasted as is). Run directories under `$REMOTE_OUT` keep the full logs; remove them only after the
report is home.
