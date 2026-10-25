---
name: ecalc-restart-note
description: "Restart note for ecalc: where the live state is (docs/STATUS.md) and how to resume"
metadata:
  node_type: memory
  type: project
  originSessionId: 774b5c91-540b-4adc-8faf-658f932531db
  modified: 2026-10-18T01:13:35.420Z
---

Live state is in `$APU/ntt/docs/STATUS.md` (running agents, aac7 queue, ETAs, next steps); decisions
in `docs/DECISIONS.md`; open work in `TASKS.md`. This note only points there (one fact, one place). Status in one
command: `$APU/bin/st`.

Latest restart point: the section "RESTART NOTE 2026-10-18 02:45 ET" at the end of docs/STATUS.md.

**Why:** the user resumes from memory without old transcripts; duplicating state here made it go stale.
**How to apply:** run `$APU/bin/state`; read docs/STATUS.md, docs/DECISIONS.md, TASKS.md. All
standing user rules (communication, security, aac7-only, no studies, act now in parallel, no invented windows) are in
the project CLAUDE.md, the single place for them; memory keeps only this pointer.
