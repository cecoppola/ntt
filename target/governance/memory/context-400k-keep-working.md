---
name: context-400k-keep-working
description: "Context cycle: at ~370k write docs/RESTART.md and keep working; auto-compaction at 400k; never stop or ask the user to clear for context size"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 920ef178-96f1-4fbb-9133-a28cbf2b1120
  modified: 2026-10-18T15:24:24.462Z
---

Context cycle (user, 2026-10-18 11:22 ET, replaces the old 400k "park and ask to clear" rule):
1. ~370k: the ctx_watch hook reminds once; write a detailed restart note to $APU/ntt/docs/RESTART.md
   (state, open jobs/agents with IDs, pending decisions, user's latest instructions, planned next tasks and work).
2. Keep working. Auto-compaction fires at ~400k (Opus 5.5 autoCompactWindow 400000 in ~/.claude/settings.json); the
   compaction guidance in $APU/CLAUDE.md covers the 370k-400k gap.
3. PreCompact hook precompact_extract.py writes exact agent/background/qadd IDs and the user's last 5 messages to
   ~/.claude/state/precompact_<session>.md; SessionStart:compact hook post_compact_inject.sh prints RESTART.md + that.

**Why:** 2026-10-18 the session parked at 02:50 ET at 400k (the hook text said "start no new work") while the user was
away; aac7 sat idle ~5.5 h. The user rejected a running task ledger as too much overhead.
**How to apply:** never stop autonomous work for context size; resume after compaction from the printed note.
Related: [[aac7-packing]], [[ecalc-restart-note]].
