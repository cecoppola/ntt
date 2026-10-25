#!/usr/bin/env python3
"""Claude Code PreToolUse guard (user instruction 2026-10-12): agents may not bypass or alter the repository leak guard."""
import json, re, sys
d = json.load(sys.stdin)
ti = d.get("tool_input", {}) or {}
cmd = ti.get("command", "") or ""
path = ti.get("file_path", "") or ti.get("notebook_path", "") or ""
bad = None
# user exemption 2026-10-25 (Q118): a git command written `TARGET_OVERRIDE=1 git ...` may skip the leak guard (target/ work)
override = bool(re.search(r"(?:^|[;&|]\s*)TARGET_OVERRIDE=1\s+git\b", cmd))
if cmd:
    if override and not re.search(r"\.git/hooks|hooksPath|GIT_HOOKS|\.config/ecalc/deny", cmd): pass
    elif re.search(r"--no-verify\b", cmd): bad = "--no-verify bypasses the leak guard"
    elif re.search(r"core\.hooksPath|GIT_HOOKS|hooksPath", cmd): bad = "changing the git hooks path disables the leak guard"
    elif re.search(r"\bgit\b[^|;&]*\bcommit\b[^|;&]*\s-[a-zA-Z]*n", cmd): bad = "git commit -n bypasses the leak guard"
    elif re.search(r"\.git/hooks|/hooks/(pre-commit|pre-push|commit-msg|secret_scan\.py)", cmd) and re.search(r"\b(rm|mv|chmod|truncate|sed\s+-i|tee|cp|ln)\b|>", cmd):
        bad = "modifying the leak-guard hooks is not allowed"
    elif re.search(r"\.config/ecalc/deny", cmd) and re.search(r"\b(rm|mv|truncate|sed\s+-i|tee|cp)\b|>", cmd):
        bad = "modifying the leak-guard deny lists is not allowed"
if path and re.search(r"/\.git/hooks/|/\.config/ecalc/deny", path):
    bad = "editing the leak guard is not allowed"
if bad:
    sys.stderr.write("BLOCKED by user policy: " + bad + ". Remove the sensitive data instead; ask the user if blocked.\n")
    sys.exit(2)
