# AGENT RULES: TOKEN EFFICIENCY & ROUTING

Project-level markdown files take precedence over this file. Project specifics (conventions, verification standards, cluster access and safety, state files) belong there, not here.

Sections: **1. All agents** · **2. Subagents** · **3. Coordinator only** (the main session that talks to the user).

---

## 1. All agents

### Reading and searching
- **Check context first:** don't re-read what is already in context. Prefer symbol/function lookups; read with line bounds (`offset`, `limit`).
- **Scope searches:** limit `grep`/`find`/tree commands to explicit subdirectories and cap output (`grep -m 20`, `find -maxdepth 3`).
- **Bound shell output:** pipe stdout/stderr through filters (`2>&1 | head -n 50`, `grep -E -C 3 "Error|Fail|Exception"`).
- **Never read:**
  - anything inside `.git/`, build output dirs, or binaries (`*.o`, `*.so`, `*.a`, executables);
  - large data/output files whole (multi-GB results, part files, `*.bundle`, core dumps);
  - a log without `head`, `tail`, or `grep`.
- **Network file systems:** avoid bulk reads over them.
- **Lazy loading:** load `docs/*.md` specs only when needed. Use quiet CLI flags (`-q`, `--silent`). Run independent tool calls in parallel.
- **Environment before code:** when something external fails, check paths, environment variables, and module availability before changing application code.

### Working and editing
- **Low chatter inside tool loops:** no preambles or narration between tool calls. This does not cancel the reports required in sections 2 and 3, and it never applies to the main session's replies to the user.
- **Brief reasoning:** stick to file anchors, edit strategy, and verification. Longer reasoning is fine for algorithm/architecture design, hard diagnosis, correctness proofs, or preparing a user decision.
- **Edits:** use the edit tools. Don't print diffs in chat; when showing a change, use a unified diff with ≤ 3 lines of context.
- **Read before editing:** read any file before you edit it.
- **Validate:** build or lint after edits and before committing.
- **Commit and push** when a long piece of work succeeds and its tests pass, with a detailed message. If the result is ambiguous or negative, present options instead.

### Long-running work (jobs, builds, test suites, remote runs: anything over a few minutes)
- **Never keep an agent alive just to wait.** Prompt caches expire after 60 minutes idle. Every wake-up re-writes the agent's whole context, usually the largest avoidable cost.
- **Fire-and-forget driver:** before a long wait, put the whole remaining sequence into one script that runs where the work runs. The script:
  - runs the steps and checks the results;
  - writes a summary file and a done-marker;
  - cleans up after itself (e.g. releases its own allocation).
- **After arming a driver:** record the marker path and the collection steps in the RESUME note, then return (`SUCCESS: armed …`).
- **Batch the queue:** chain dependent runs inside one driver or allocation, not one agent turn per run.
- **Collect cheaply:**
  - check the marker with one bounded command;
  - collect with that command or a fresh `haiku` that reads the summary file;
  - never wake a large agent for this.
- **If waiting is truly unavoidable:** use one blocking background command that exits only on completion or failure, at the longest interval the work allows. No `Monitor`, per-line notifications, or short polling loops.

---

## 2. Subagents

### Inputs
- **Your prompt is your whole context:** you don't see the parent's conversation.
- **Project rules:** the project's CLAUDE.md is already loaded; read any agent protocol file it names before starting.

### Return protocol
- **Return exactly once:** when the task is finished or blocked. Each stop wakes the parent at the cost of re-reading its whole context, so:
  - no interim "waiting" or "still running" stops;
  - never repeat a report.
- **Report as short bullets:**
  - **From / task:** who tasked you and the assignment.
  - **Result:** `SUCCESS`, `FAILED`, or `BLOCKED`, with the reason.
  - **Files:** files modified, and the path to any report file.
  - **Numbers:** key numbers, each labelled *measured*, *modelled*, or *assumed* (omit if none).
  - **Decisions:** decisions for the user, each option with its cost/benefit. If none, write `Decisions: none`. Don't invent any.
- **Keep the return small:** no raw tool output, full files, or intermediate reasoning. Long detail goes in the report file.
- **Report doubt:** if you were unsure of your result, say so in the Result line. The coordinator uses this to decide whether to escalate.

---

**Main session only:** your coordinator rules (reporting, routing, delegating, sessions/state, named agents, push-down, 400k rule) are in `~/.claude/COORDINATOR.md`, injected at session start. If you are the main session and do not see them, read that file first. Subagents ignore this.
