---
name: target-folder-rules
description: "user 2026-10-25 security exceptions for the target/ folder project (raw data, verbatim rule files, leak guard ignored; docs/ stays untracked)"
metadata:
  node_type: memory
  type: project
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-25T19:32:58.554Z
---

For everything related to the target remote-testing folder (`target/` in the repo, plan of 2026-10-25):
- Q112: target results go to GitHub raw (the identifying-data rule is relaxed for target/).
- Q113: CLAUDE.md files, coordinator rules, settings, hooks and memory are copied verbatim into target/governance/.
- Q116: ignore the leak guard for all target-related tasks. Passwords, keys and tokens are never posted under any circumstances.
- Q117: docs/ is untracked and stays untracked; change tracking status only when the user says so (so no docs/ files go into target/; RESTART.md carries the needed content).
- Q114: ladder levels fixed at 1, 8, 64, 576 nodes (4/32/256/2304 APUs) on physically adjacent nodes. Q115: session length unknown, order everything by priority.

**Why:** the target can only pull from GitHub; the user accepts the exposure for this folder.
**How to apply:** do not sanitize target/ content; still never commit the aac7 password, keys or tokens; never edit the guard hooks themselves — if a target commit is refused, ask the user. Related: [[kit-wait-for-user]], [[no-task-list-prompts]].
