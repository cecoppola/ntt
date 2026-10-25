# target/RESTART.md — start here on the target system

Scope: everything an agent needs to start work on the target system at once: the mission, the rules and how to install
them, the project as we understand it, and steps 1-4 with exact commands. Written 2026-10-25 (Eastern time) by the
coordinator on the development side, which has no access to the target. Conventions: `target/FORMATS.md`.

> DRAFT — sections marked [OPEN] are filled before the push.

## 0. First five minutes

1. `git clone` this repository (or `git pull` the existing clone) on the target login node; `cd` into it.
   `APU=$(dirname "$PWD")` — the work root (the parent of the clone; it plays the role of the development side's work root).
2. Install the rules and memory (§2). Start the coordinator session (Claude Code) in the clone.
3. Read this file to the end, then `target/FORMATS.md`, then `target/README.md`.
4. Start a log: `target/results/LOG.md` (dated lines, times with the time zone).
5. Run step 1 (§5) now; it takes minutes and everything else depends on it.

## 1. Mission (the user's request of 2026-10-25, verbatim)

> (1) A new folder in the repository specifically built for all our target remote testing and evaulation. This will
> contain information we put into it now, and it will also store information we collect during the tests that we will
> use later. The remaining work items all go into this folder.
> (2) A set of quick system tests to collect all of the critical information about the target system. This includes all
> filepaths, libraries, version numbers, hardware information, network architecture, queue system and rules, and
> everything else you will need to work. You will use this data to produce the target-specific machine configuration.
> These must all be fast callsw, and you can write a script and/or instructions for them.
> (3) A set of short and medium-length benchmarks, parameter sweeps, and tests, which you will use to answer every
> outstanding question we have about how to optimize the algorithm for the target system. This must be designed to run
> on a team of many agents working in parallel to collect the information without bottlenecking any tests. These must
> be designed to be fast, not exhaustive, so do not run them multiple times and write them to be efficient. These must
> also measure every piece of information you could possibly need in order to continue doing testing and development on
> this system, after the target access is over, and we are doing development without direct access to the system.
> (4) A nearly-complete version of ecalc that you will have ready to run on the system after (2) & (3). You will design
> this to run on 1 node, 1 chassis, 1 rack, and then the entire system. For the HPE cabinets we are dealing with, I
> believe that is 4 APUs, 32 APUs, 256 APUs, and 2304 APUs. I want you to figure out how to index the nodes based on
> their physical location and run the groups according to their physical locality. Be prepared to run these in order,
> where you run it a few times at the smallest level, measure the results, make adjustments, and then move to the next
> size level. These must be ready to run quickly, in order. For the lower 3 levels, you can run multiple identical jobs
> in different queues since there will be many nodes available.
> (5) You will also need to write a detailed, fully complete RESTART.md file that contains every piece of information
> that an agent will need to immediately spring into action on the target system, including these instructions (1),
> (2), (3), and (4). This file should contain your understanding of the project and your context for continuing this
> work there. You also need to copy this system's claude.md and other governing settings files into this folder and
> instruct the agent, in the RESTART.md file, to copy all of those to the correct locations on the target system.

The user's decisions on this project (2026-10-25):

| id | decision |
|---|---|
| Q112 | raw target data is committed to `target/results/` as measured (the no-identifying-data rule is relaxed for this folder) |
| Q113 | the governing files are copied verbatim into `target/governance/` |
| Q114 | ladder sizes fixed: 1, 8, 64, 576 nodes (4 / 32 / 256 / 2304 APUs), each group on physically adjacent nodes |
| Q115 | session length unknown: order everything by priority, so any stopping point leaves the most important answers |
| Q116 | ignore the leak guard for target work; never post passwords, keys or tokens under any circumstances |
| Q117 | `docs/` is untracked and stays so; this file carries what the target agent needs from it |
| Q118 | the governance copies are exact; the user overrode the development side's commit guard for them ("My instructions supersede everything else") |

## 2. Install the rules (do this first)

`target/governance/` holds the development side's rule files. Install them with
`bash target/governance/install.sh` from the clone's root (existing files are kept and the copy is put next to them as
`<name>.from-dev`; `--force` overwrites). It places:

| file in `target/governance/` | installed as |
|---|---|
| `apucode_CLAUDE.md` | `$APU/CLAUDE.md` (project rules; Claude Code reads it for every session under `$APU`) |
| `home_claude/CLAUDE.md` | `~/.claude/CLAUDE.md` (the user's global agent rules) |
| `home_claude/COORDINATOR.md` | `~/.claude/COORDINATOR.md` (injected into the main session by a hook) |
| `home_claude/settings.json` | `~/.claude/settings.json` (hooks, model settings, env; merge by hand if the target has its own) |
| `home_claude/hooks/*` | `~/.claude/hooks/` (executable) |
| `home_claude/agents/*.md` | `~/.claude/agents/` (the four agent types: haiku-scout, haiku-worker, sonnet-developer, sonnet-reviewer) |
| `memory/*.md` | `~/.claude/projects/<key>/memory/`, `<key>` = the clone's absolute path with every `/` replaced by `-` |
| `project_claude/settings.local.json` | `<clone>/.claude/settings.local.json` |

The copies are exact. They name the development side's work root (`/home/machinus/apucode`); the script rewrites it to
the target's work root `$APU` in the installed project rules and memory.
Restart Claude Code after installing so the hooks load.

How the rules apply on the target (they were written for the development side):
- **Target rules win** where they differ: this file and the Q112-Q118 decisions above.
- Rules about the development cluster `aac7` (its queue tool `aac7q`, `qadd`, `bin/c7`, `bin/cp7`, session holds,
  the 6-node packing limit, the NIC-loss workarounds, the password file) do not apply on the target; their intent does:
  pack work so no allocated node idles, use long allocations and run many items inside them, drivers with done-markers
  for long waits, never touch other users' jobs, kill only by single PID.
- The private `docs/` files (STATUS, DECISIONS, AGENT_PROTOCOL) do not exist in the clone (Q117). On the target the
  coordinator keeps its live state in `target/results/LOG.md` and decisions in `target/results/DECISIONS.md` (create both).
- The local leak-guard git hooks are not part of the clone; on the target nothing blocks a commit, so the coordinator
  itself makes sure no password, key or token is ever committed (Q116). Raw data (host names, paths, versions) is fine (Q112).
- Correctness first, labels m / mod / a, gates before merging code changes (`TESTING.md`), priorities, parallel work:
  all apply unchanged.

## 3. The project as we understand it

[OPEN: summary of ecalc, the two paths, current figures, memory fit, the time estimate, the known risks]

## 4. How to work on the target: the team

[OPEN: coordinator + workers, allocations, the commit cadence, priorities, what to send back]

## 5. Step 1 — system facts (minutes)

[OPEN: probe commands, gen_conf]

## 6. Step 2 — build

[OPEN]

## 7. Step 3 — the bench (parallel workers)

[OPEN]

## 8. Step 4 — the ladder (1, 8, 64, 576 nodes)

[OPEN]

## 9. Known problems and their fixes

[OPEN]

## 10. Before access ends

- Commit and push `target/results/` (the coordinator, every 30-60 min and now).
- `bench/aggregate.py`; check `results/bench/ANSWERS.md` and `METRICS.tsv` are committed.
- Write `target/results/HANDOFF.md`: what ran, what is left (by priority), surprises, the next questions.
