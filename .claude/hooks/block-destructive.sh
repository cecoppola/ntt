#!/bin/bash
# =============================================================================
# HOOK: block-destructive.sh
# EVENT: PreToolUse (Bash)
# PURPOSE: Blocks destructive or hard-to-reverse shell commands. These commands
#          can permanently delete files, overwrite git history, or break shared
#          state. This hook forces you to approve them manually instead of
#          letting Claude run them autonomously.
#
# BEHAVIOR: Scans the Bash command for known destructive patterns. If found,
#          exits with code 2 (block), sending an explanation to Claude. Claude
#          then asks you for explicit permission before proceeding.
#
# HOW TO ADJUST THIS HOOK:
#   - To ALLOW a blocked pattern: remove it from DESTRUCTIVE_PATTERNS.
#   - To BLOCK an additional pattern: add a regex to DESTRUCTIVE_PATTERNS.
#   - To DISABLE entirely: set HOOK_ENABLED=0.
#   - To make this a WARNING only (proceed but notify): set EXIT_CODE=0.
#   - To narrow a pattern (e.g., block "rm -rf /" but not "rm -rf ./build"):
#     replace the broad pattern with a more specific regex.
# =============================================================================

# ── TUNABLE VARIABLES ────────────────────────────────────────────────────────

# Master switch. 0 = disabled (allow all), 1 = enabled (enforce blocking).
HOOK_ENABLED=1

# Exit code when a destructive command is detected.
# 2 = block and send feedback to Claude (Claude asks user before retrying).
# 0 = warn only (Claude sees the message but the command still runs) — NOT recommended.
EXIT_CODE=2

# Patterns to block. These are extended regex (ERE) matched against the full command.
# Each pattern should be specific enough to avoid false positives.
# Use anchors, word boundaries, and flags to keep patterns tight.
DESTRUCTIVE_PATTERNS=(
    # File deletion — broad recursive deletes
    "rm[[:space:]]+-[rRf]*rf"          # rm -rf, rm -fr, rm -Rf variants
    "rm[[:space:]]+-[rR][[:space:]]"   # rm -r <anything>
    "rm[[:space:]]+-f[[:space:]]"      # rm -f <anything>

    # Git history rewriting — these modify shared history
    "git[[:space:]]+push[[:space:]]+.*--force"   # git push --force
    "git[[:space:]]+push[[:space:]]+-f"          # git push -f
    "git[[:space:]]+reset[[:space:]]+--hard"     # git reset --hard
    "git[[:space:]]+rebase[[:space:]]+-i"        # interactive rebase (modifies history)
    "git[[:space:]]+commit[[:space:]]+.*--amend" # amend (rewrites last commit)
    "git[[:space:]]+clean[[:space:]]+-f"         # git clean -f (deletes untracked files)

    # Overwriting files — redirect to file without confirmation
    ">[[:space:]]*/home/"              # overwrite files in home directory
    ">[[:space:]]*/etc/"              # overwrite system config files

    # Package/environment destruction
    "pip[[:space:]]+uninstall[[:space:]]+-y" # silent pip uninstall
    "conda[[:space:]]+env[[:space:]]+remove" # remove conda environment

    # Process killing
    "kill[[:space:]]+-9[[:space:]]+1$"   # kill init (PID 1)
    "killall[[:space:]]+-9"              # kill all processes matching name
)

# Patterns to explicitly allow even if they match a blocked pattern above.
# Use these to carve out safe exceptions to the rules above.
ALLOWED_PATTERNS=(
    # Example: allow rm -rf on the build directory specifically
    # "rm[[:space:]]+-rf[[:space:]]+\./build"
    # "rm[[:space:]]+-rf[[:space:]]+/tmp/"
)

# Message sent to Claude when a destructive command is blocked.
# Make it actionable: tell Claude what to do instead (ask the user explicitly).
BLOCK_MESSAGE="Blocked: this command is potentially destructive or hard to reverse. Ask the user for explicit confirmation before running it, and describe exactly what it will do and what cannot be undone."

# ── HOOK LOGIC ───────────────────────────────────────────────────────────────

if [ "$HOOK_ENABLED" -eq 0 ]; then
    exit 0
fi

INPUT=$(cat)
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty' 2>/dev/null)

if [ -z "$COMMAND" ]; then
    exit 0
fi

# Check allowed patterns first
for allow_pat in "${ALLOWED_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -qE "$allow_pat" 2>/dev/null; then
        exit 0
    fi
done

# Check destructive patterns
for pattern in "${DESTRUCTIVE_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -qE "$pattern" 2>/dev/null; then
        echo "BLOCKED COMMAND: $COMMAND" >&2
        echo "$BLOCK_MESSAGE" >&2
        exit "$EXIT_CODE"
    fi
done

exit 0
