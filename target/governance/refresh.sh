#!/bin/bash
# target/governance/refresh.sh - DEVELOPMENT SIDE ONLY: refresh the verbatim copies from the live rule files (install.sh is the
# target-side counterpart). Run from the repository root: bash target/governance/refresh.sh
# A source holding a real secret pattern (private key block, sshpass -p, API token prefixes, password=value) is skipped and reported.
set -e
G=target/governance; [ -d "$G" ] || { echo "run from the repository root"; exit 1; }
W=/home/machinus/apucode; H=/home/machinus/.claude; M=$H/projects/-home-machinus-apucode-ntt/memory
SECRET='BEGIN [A-Z ]*PRIVATE KEY|sshpass -p|ghp_[A-Za-z0-9]{20}|sk-ant-[A-Za-z0-9]|(password|passwd|token|secret)[A-Za-z_]*[=:] *[^ ]{6}'
n=0
put() { # src dst
    [ -f "$1" ] || return 0
    if grep -qEi "$SECRET" "$1"; then echo "SKIPPED (secret pattern): $1"; return 0; fi
    mkdir -p "$(dirname "$2")"
    cmp -s "$1" "$2" && return 0
    cp "$1" "$2"; echo "refreshed $2"; n=$((n+1))
}
put $W/CLAUDE.md $G/apucode_CLAUDE.md
put $H/CLAUDE.md $G/home_claude/CLAUDE.md
put $H/COORDINATOR.md $G/home_claude/COORDINATOR.md
put $H/settings.json $G/home_claude/settings.json
put $W/ntt/.claude/settings.local.json $G/project_claude/settings.local.json
for f in $H/agents/*.md; do put "$f" $G/home_claude/agents/$(basename "$f"); done
for f in $H/hooks/*.sh $H/hooks/*.py; do put "$f" $G/home_claude/hooks/$(basename "$f"); done
for f in $M/*.md; do put "$f" $G/memory/$(basename "$f"); done
for f in $G/memory/*.md; do [ -e "$M/$(basename "$f")" ] || echo "stale copy (source gone): $f"; done
echo "$n file(s) refreshed"
