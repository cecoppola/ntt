---
name: item-ref-short-sha
description: aac7 queue items must be generated with a short (7-char) commit ref; a full 40-char sha fails the worktree check
metadata:
  type: project
---
Generators in day/multi (gen_*.sh via mkitem.py) must get a SHORT commit ref (e.g. 6b51c6c). The item's worktree check is
`case "$COMMIT" in <REF>*)` where COMMIT is the 8-char build key, so a full 40-char sha never matches: the item dies at
"FAILED at worktree: built 6b51c6c3, wanted 6b51c6c3d329..." after the build (9x3_s2fixgate, 2026-10-22 04:44).

**Why:** cost a queue slot and ~5 min; agents pass full shas from `git rev-parse HEAD`.
**How to apply:** tell agents to pass `git rev-parse --short=7`; check `grep NL_GITREF= items/<item>.sh` before queueing.
