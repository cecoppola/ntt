#!/bin/bash
# SessionStart hook: stop orphaned copies of a VS Code Claude session.
# When the VS Code extension host restarts, its claude child (native-binary/claude, stream-json
# mode) can be reparented to the systemd user manager (or PID 1) and keep running the same
# conversation while the reloaded panel starts a second process. This kills, by single PID,
# every such orphan of this user whose cwd equals the new session's cwd. Terminal sessions and
# daemon-hosted sessions are never touched (they are not stream-json children of systemd).
# KILL_ORPHAN_DRYRUN=1 lists candidates without killing.
input=$(cat)
cwd=$(printf '%s' "$input" | python3 -Ic 'import json,sys
try: print(json.load(sys.stdin).get("cwd",""))
except Exception: print("")' 2>/dev/null)
[ -n "$cwd" ] || cwd=$PWD
log=$HOME/.claude/hooks/kill-orphan-claude.log

# ancestors of this hook (the live session itself) are never candidates
declare -A mine
p=$$
while [ "${p:-0}" -gt 1 ]; do mine[$p]=1; p=$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' '); done

killed=()
for pid in $(pgrep -u "$(id -u)" -f 'native-binary/claude'); do
  [ -n "${mine[$pid]}" ] && continue
  # the program itself must be the claude binary (not a shell whose text mentions it)
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */native-binary/claude) ;; *) continue;; esac
  args=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null) || continue
  case "$args" in *--input-format\ stream-json*) ;; *) continue;; esac
  [ "$(readlink /proc/$pid/cwd 2>/dev/null)" = "$cwd" ] || continue
  ppid=$(ps -o ppid= -p "$pid" | tr -d ' ')
  if [ "$ppid" != 1 ]; then
    pargs=$(tr '\0' ' ' < /proc/$ppid/cmdline 2>/dev/null)
    case "$pargs" in *systemd\ --user*) ;; *) continue;; esac
  fi
  if [ -n "$KILL_ORPHAN_DRYRUN" ]; then echo "would kill $pid ($args)"; continue; fi
  kill -TERM "$pid" 2>/dev/null && killed+=("$pid")
  echo "$(date '+%F %T') killed orphan $pid cwd=$cwd ppid=$ppid" >> "$log"
done

if [ ${#killed[@]} -gt 0 ]; then
  printf '{"systemMessage":"Stopped orphaned Claude process(es) %s left running in this directory by an earlier VS Code window."}\n' "${killed[*]}"
fi
exit 0
