---
name: vscode-exthost-oom
description: "Duplicate Claude sessions came from VS Code extension-host OOM crashes (likely the Claude Status extension, disabled 2026-10-18); orphan-kill SessionStart hook in place"
metadata:
  node_type: memory
  type: project
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-18T14:58:37.202Z
---

2026-10-17/18: 11 VS Code extension-host crashes (exit 133, V8 "Reached heap limit", ~3.7 GB in large-object
space; minidumps in ~/.config/Code/Crashpad/completed). Each crash orphaned the running Claude process (reparented to
systemd --user), which kept working while the reopened panel resumed the same conversation: duplicate sessions.
Likely cause: the third-party extension long-kudo.vscode-claude-status ("Claude Status", installed 2026-10-17 14:08),
which re-reads every ~/.claude/projects/**/*.jsonl (227 MB) on each transcript change. User disabled it 2026-10-18
10:57 ET. Safety net: SessionStart hook ~/.claude/hooks/kill-orphan-claude.sh (log next to it).

**Why:** if crashes recur with the extension disabled, the suspect was wrong; check renderer.log and the minidumps.
**How to apply:** on "duplicate session" reports, check the extension-host crash log first.
