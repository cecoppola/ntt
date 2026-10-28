# target/ — remote testing and evaluation on the target system

> **Which workflow governs.** The target session is copy-and-paste only (the user unpacks a ZIP, no git, no agent on the target; a browser agent reads GitHub and gives command
> blocks): `remote/AGENT.md` governs. The Claude Code workflow described below (run order step 0 and `RESTART.md` §0, §2: `git clone`, installing rules, LOG.md) is not used and its files are not in the copy.

Scope: everything for the target session (576 nodes × 4 MI300A, 8 Slingshot NICs per node, Cray SHMEM 12.1.0): what we
bring (scripts, the ecalc build, rules) and what we collect (raw results, committed as measured). An agent on the target
starts with `RESTART.md`; conventions are in `FORMATS.md`.

## Run order (each step is ordered by priority, so the session can end at any point)

| step | what | where | time |
|---|---|---|---|
| 0 | (Claude-Code workflow only, not used in the paste session) install the rules and memory | login node | 5 min |
| 1 | system facts: `probe/probe.sh` on a login node, then inside a one-node job; `probe/gen_conf.py` → `config/target.conf` | login + 1 node | ≤ 10 min |
| 2 | build path 1 and path 2 (bench items `bld01`, `bld02`) | login node | ~5 min |
| 3 | bench: workers loop `bench/run.sh` in parallel allocations; answers in `results/bench/ANSWERS.md` | 1-64 nodes each | minutes per item |
| 4 | ladder: `ladder/ladder.sh --level L0` (a few runs, adjust per `ladder/ADJUST.md`), then L1, L2, L3 | 1 / 8 / 64 / 576 nodes | per level |

## Folder map

See `FORMATS.md` §1.
