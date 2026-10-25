#!/usr/bin/env python3
"""PreCompact: pull exact facts a compaction summary tends to lose out of the transcript (agents and background
commands with IDs and status, aac7 queue submissions, the user's latest messages verbatim) into
~/.claude/state/precompact_<session>.md; post_compact_inject.sh prints it after compaction. Fail-open, never blocks."""
import json, os, re, sys, time
from datetime import datetime, timedelta, timezone
try:
    from zoneinfo import ZoneInfo; ET = ZoneInfo("America/New_York")
except Exception:
    ET = None

def et(ts):
    try:
        d = datetime.fromisoformat(ts.replace("Z", "+00:00"))
        return (d.astimezone(ET) if ET else d).strftime("%m-%d %H:%M")
    except Exception:
        return "?"

def text_of(c):
    if isinstance(c, str): return c
    if isinstance(c, list):
        return "\n".join(x.get("text", "") if isinstance(x, dict) and x.get("type") == "text" else
                         (text_of(x.get("content")) if isinstance(x, dict) and x.get("type") == "tool_result" else "")
                         for x in c)
    return ""

try:
    d = json.load(sys.stdin)
    tp = d.get("transcript_path") or ""
    if not tp or not os.path.exists(tp): sys.exit(0)
    cutoff = datetime.now(timezone.utc) - timedelta(hours=48)
    agents, bgs, qadds, users = {}, {}, [], []   # id -> dict
    pending = {}                                  # tool_use_id -> (kind, info)
    def notify(s, ts):                            # task-notifications only (never tool output that quotes one)
        if not s.lstrip().startswith("<task-notification>"): return
        for mm in re.finditer(r"<task-id>(\w+)</task-id>.*?<status>(\w+)</status>", s, re.S):
            tid, st = mm.groups()
            for tbl in (agents, bgs):
                if tid in tbl: tbl[tid]["status"] = st + " " + et(ts)
    # background commands and in-process agents die with the Claude process: find when the live one started
    born = None
    try:
        p = os.getppid()
        while p > 1:
            if "native-binary/claude" in os.readlink("/proc/%d/exe" % p) or b"claude" in open("/proc/%d/cmdline" % p, "rb").read().split(b"\0")[0]:
                tick = int(open("/proc/%d/stat" % p).read().rsplit(")", 1)[1].split()[19])
                up = float(open("/proc/uptime").read().split()[0])
                born = datetime.now(timezone.utc) - timedelta(seconds=up - tick / os.sysconf("SC_CLK_TCK")); break
            p = int(open("/proc/%d/stat" % p).read().rsplit(")", 1)[1].split()[1])
    except Exception:
        born = None
    for line in open(tp, encoding="utf-8", errors="replace"):
        try: r = json.loads(line)
        except Exception: continue
        if r.get("isSidechain"): continue
        ts = r.get("timestamp") or ""
        try:
            if datetime.fromisoformat(ts.replace("Z", "+00:00")) < cutoff: continue
        except Exception:
            pass
        m = r.get("message") or {}; c = m.get("content")
        if r.get("type") == "assistant" and isinstance(c, list):
            for x in c:
                if not isinstance(x, dict) or x.get("type") != "tool_use": continue
                n, i = x.get("name"), x.get("input") or {}
                if n == "Agent":
                    pending[x.get("id")] = ("agent", {"t": et(ts), "desc": i.get("description", ""),
                                                      "type": i.get("subagent_type") or i.get("model") or "general"})
                elif n == "Bash" and i.get("run_in_background"):
                    pending[x.get("id")] = ("bg", {"t": et(ts), "desc": i.get("description", ""),
                                                   "cmd": i.get("command", "")[:120]})
                elif n == "Bash" and re.search(r"\bqadd\b", i.get("command", "")):
                    qadds.append((et(ts), i.get("command", "").replace("\n", " ")[:160]))
                elif n == "SendMessage":
                    to = i.get("to", "")
                    if to in agents: agents[to]["status"] = "resumed " + et(ts)
        elif r.get("type") == "user":
            if isinstance(c, list):
                for x in c:
                    if isinstance(x, dict) and x.get("type") == "tool_result" and x.get("tool_use_id") in pending:
                        kind, info = pending.pop(x.get("tool_use_id"))
                        t = text_of(x.get("content"))
                        mm = re.search(r"agentId: (\w+)", t) if kind == "agent" else re.search(r"background with ID: (\w+)", t)
                        if mm:
                            try: dead = born and datetime.fromisoformat(ts.replace("Z", "+00:00")) < born
                            except Exception: dead = False
                            info["status"] = "lost (Claude process restarted)" if dead else "running"
                            (agents if kind == "agent" else bgs)[mm.group(1)] = info
                        elif kind == "agent":   # foreground agent: already returned
                            info["status"] = "returned " + et(ts); agents["fg-" + info["t"]] = info
            s = c if isinstance(c, str) else "\n".join(x.get("text", "") for x in c or []
                                                         if isinstance(x, dict) and x.get("type") == "text")
            notify(s, ts)
            if (r.get("origin") or {}).get("kind") == "human" or r.get("promptSource") == "user":
                if s.strip(): users.append((et(ts), s.strip()))
        elif r.get("type") == "attachment":
            notify(str((r.get("attachment") or {}).get("prompt", "")), ts)
        elif r.get("type") == "queue-operation" and r.get("operation") == "enqueue":
            notify(str(r.get("content", "")), ts)
    out = ["# Pre-compaction extract (exact, from the transcript; written %s)" %
           ((datetime.now(ET) if ET else datetime.now()).strftime("%Y-%m-%d %H:%M ET")), ""]
    def tbl(title, rows, fmt):
        out.append("## " + title)
        out.extend(fmt(k, v) for k, v in rows) if rows else out.append("(none)")
        out.append("")
    is_open = lambda v: v.get("status", "").split(" ")[0] in ("running", "resumed")
    ag = list(agents.items())
    tbl("Agents still open (resume with SendMessage to the ID)", [(k, v) for k, v in ag if is_open(v)],
        lambda k, v: "- %s | %s | %s | %s | %s" % (v["t"], v["type"], v["desc"], k, v["status"]))
    tbl("Agents finished (last 8)", [(k, v) for k, v in ag if not is_open(v)][-8:],
        lambda k, v: "- %s | %s | %s | %s" % (v["t"], v["type"], v["desc"], v["status"]))
    bg = list(bgs.items())
    tbl("Background commands still running", [(k, v) for k, v in bg if is_open(v)],
        lambda k, v: "- %s | %s | %s | %s" % (v["t"], v["desc"], k, v["cmd"]))
    tbl("aac7 queue submissions (last 10; check bin/state for their state)", list(enumerate(qadds[-10:])),
        lambda k, v: "- %s | %s" % v)
    tbl("User's last 5 messages (verbatim, truncated at 1500 chars)", list(enumerate(users[-5:])),
        lambda k, v: "- [%s] %s" % (v[0], v[1][:1500].replace("\n", "\n  ")))
    sd = os.path.expanduser("~/.claude/state"); os.makedirs(sd, exist_ok=True)
    open(os.path.join(sd, "precompact_%s.md" % d.get("session_id", "x")), "w").write("\n".join(out))
except Exception:
    sys.exit(0)
