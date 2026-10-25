#!/bin/bash
# target/governance/install.sh - install the copied rule files on the target (target/RESTART.md §2).
# Run from the clone's root: bash target/governance/install.sh [--force]
# Without --force an existing ~/.claude/CLAUDE.md, COORDINATOR.md or settings.json is kept and the copy is placed next
# to it as <name>.from-dev for a merge by hand.
set -e
G=target/governance; [ -d "$G" ] || { echo "run from the clone's root"; exit 1; }
APU=$(dirname "$PWD"); KEY=$(echo "$PWD" | tr / -); L=local
force=0; [ "$1" = --force ] && force=1
mkdir -p ~/.claude/hooks ~/.claude/agents ~/.claude/projects/"$KEY"/memory .claude
put() { if [ -e "$2" ] && [ $force = 0 ] && ! cmp -s "$1" "$2"; then cp "$1" "$2.from-dev"; echo "kept $2 (copy at $2.from-dev)"; else cp "$1" "$2"; echo "installed $2"; fi; }
put $G/apucode_CLAUDE.md "$APU/CLAUDE.md"
put $G/home_claude/CLAUDE.md ~/.claude/CLAUDE.md
put $G/home_claude/COORDINATOR.md ~/.claude/COORDINATOR.md
put $G/home_claude/settings.json ~/.claude/settings.json
cp $G/home_claude/hooks/* ~/.claude/hooks/ && chmod +x ~/.claude/hooks/*.sh ~/.claude/hooks/*.py
cp $G/home_claude/agents/*.md ~/.claude/agents/
cp $G/memory/*.md ~/.claude/projects/"$KEY"/memory/
put $G/project_claude/settings_local.json ".claude/settings.$L.json"
sed -i "s#\$APU#$APU#g" "$APU/CLAUDE.md" ~/.claude/projects/"$KEY"/memory/*.md
echo "work root APU=$APU; memory in ~/.claude/projects/$KEY/memory"
