#!/bin/sh
# SessionStart (compact): put the project's restart note (<project>/docs/RESTART.md) and the pre-compaction extract
# (precompact_extract.py) into the new context, so the session resumes from them. Fail-open.
input=$(cat)
eval "$(printf '%s' "$input" | python3 -Ic 'import json,sys,shlex
try: d=json.load(sys.stdin)
except Exception: d={}
print("sid=%s cwd=%s" % (shlex.quote(d.get("session_id","x")), shlex.quote(d.get("cwd",""))))' 2>/dev/null)"
dir=${CLAUDE_PROJECT_DIR:-${cwd:-$PWD}}
note="$dir/docs/RESTART.md"
ext="$HOME/.claude/state/precompact_${sid:-x}.md"
if [ -f "$note" ]; then
  echo "# RESTART NOTE ($note, last written $(date -r "$note" '+%Y-%m-%d %H:%M %Z'); if that is not from just before this compaction, treat it as stale)"
  echo "Resume from this note. The compaction summary and the extract below cover what happened after it was written."
  echo
  cat "$note"
  echo
fi
[ -f "$ext" ] && cat "$ext"
exit 0
