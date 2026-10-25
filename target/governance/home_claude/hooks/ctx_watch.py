#!/usr/bin/env python3
"""PostToolUse: once per context cycle, at 370k, remind the main session to write its restart note before auto-compaction (400k). Fail-open, never blocks."""
import json, os, sys
try:
    d = json.load(sys.stdin)
    if d.get("agent_id"): sys.exit(0)           # subagent call
    tp = d.get("transcript_path") or ""
    if not tp or "/subagents/" in tp or not os.path.exists(tp): sys.exit(0)
    size = os.path.getsize(tp)
    with open(tp, "rb") as f:
        f.seek(max(0, size - 400000)); tail = f.read().decode("utf-8", "replace").splitlines()
    ctx = 0
    for l in reversed(tail):
        if '"usage"' not in l: continue
        try: r = json.loads(l)
        except Exception: continue
        m = r.get("message") or {}; u = m.get("usage") or {}
        if r.get("type") == "assistant" and u and not r.get("isSidechain") and m.get("model") != "<synthetic>":
            ctx = u.get("input_tokens", 0) + u.get("cache_read_input_tokens", 0) + u.get("cache_creation_input_tokens", 0); break
    LIM = int(os.environ.get("CTX_WATCH_LIMIT", "370000"))
    sf = os.path.expanduser("~/.claude/state/ctx_watch_%s.txt" % d.get("session_id", "x"))
    if ctx < LIM:
        if os.path.exists(sf): os.remove(sf)   # context was cleared/compacted: re-arm
        sys.exit(0)
    if os.path.exists(sf): sys.exit(0)        # already reminded in this context cycle
    os.makedirs(os.path.dirname(sf), exist_ok=True); open(sf, "w").write(str(ctx))
    msg = ("CONTEXT WATCH: main-session context is ~%dk tokens; auto-compaction fires at ~400k. If you are the main session: now write "
           "the detailed restart note (state, open jobs and agents with IDs, pending decisions, the user's latest instructions, planned next "
           "tasks and work) to the project's restart-note file (<project>/docs/RESTART.md, overwrite it), then KEEP WORKING; do not stop or "
           "ask the user to clear. Subagents: ignore." % (ctx // 1000))
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": msg}}))
except Exception:
    sys.exit(0)
