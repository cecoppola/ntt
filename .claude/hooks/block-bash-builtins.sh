#!/bin/bash
# =============================================================================
# HOOK: block-bash-builtins.sh
# EVENT: PreToolUse (Bash)
# PURPOSE: Forces Claude to use dedicated tools (Grep, Read, Glob) instead of
#          shelling out to grep/cat/find/head/tail/sed/awk. Dedicated tools are
#          more token-efficient, produce bounded output, and have better UX.
#
# BEHAVIOR: When Claude runs a Bash command matching a blocked pattern, this
#          hook exits with code 2 (block) and sends an explanation back to
#          Claude so it can switch to the correct tool instead.
#
# HOW TO ADJUST THIS HOOK:
#   - To ALLOW a command that is currently blocked: remove it from BLOCKED_CMDS.
#   - To BLOCK an additional command: add it to BLOCKED_CMDS.
#   - To DISABLE this hook entirely: set HOOK_ENABLED=0 below.
#   - To make the block a WARNING only (Claude sees the message but proceeds):
#     change EXIT_CODE from 2 to 0 and set WARN_ONLY=1.
#   - To allow a blocked command in specific circumstances, add an exception
#     pattern to ALLOWED_EXCEPTIONS below.
# =============================================================================

# ── TUNABLE VARIABLES ────────────────────────────────────────────────────────

# Master switch. Set to 0 to disable this hook entirely and allow all commands.
# Change to: 0 = disabled (allow everything), 1 = enabled (enforce blocking)
HOOK_ENABLED=1

# Exit code when a command is blocked.
# 2 = block the tool call and send stderr as feedback to Claude (recommended).
# 0 = allow the tool call but still send the feedback message (warning-only mode).
EXIT_CODE=2

# Commands to block. These are checked as the first word of the Bash command.
# Each entry is a plain string matched against the start of the command.
# Add or remove entries to control which commands are blocked.
BLOCKED_CMDS=(
    "grep"     # Use the Grep tool instead — supports regex, bounded output
    "rg"       # ripgrep — use the Grep tool instead
    "cat"      # Use the Read tool instead — handles line ranges natively
    "head"     # Use the Read tool with limit= instead
    "tail"     # Use the Read tool with offset= instead
    "find"     # Use the Glob tool instead — pattern matching with less output
    # "ls" is allowed — Glob requires a pattern; ls is needed for bare directory listing
    "sed"      # Use the Edit tool for file edits; Grep for searches
    "awk"      # Use the Grep tool or Read tool instead
)

# Exception patterns: if the Bash command contains any of these substrings,
# allow it even if it starts with a blocked command. Use this to permit
# specific legitimate uses of otherwise-blocked tools.
# Example: "grep -c" would allow grep used only for counting matches.
ALLOWED_EXCEPTIONS=(
    # Add exception patterns here, one per line:
    # "grep.*wc"          # allow grep piped to wc
    # "cat /proc/"        # allow reading /proc pseudo-files
)

# Message sent to Claude when a command is blocked.
# Claude reads this and adjusts its approach. Be specific about what to use instead.
# Changing this message changes the guidance Claude receives — make it actionable.
BLOCK_MESSAGE="Use the dedicated Grep/Read/Glob tools instead of shell commands for file search and inspection. These tools are more token-efficient and produce bounded output. Only use Bash for build, compile, run, and profile commands."

# ── HOOK LOGIC (do not edit below unless you know what you are doing) ────────

if [ "$HOOK_ENABLED" -eq 0 ]; then
    exit 0
fi

INPUT=$(cat)
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty' 2>/dev/null)

if [ -z "$COMMAND" ]; then
    exit 0
fi

# Extract the first word of the command (strip leading whitespace and pipes)
FIRST_WORD=$(echo "$COMMAND" | sed 's/^[[:space:]]*//' | awk '{print $1}' | sed 's/.*\///')

# Check exceptions first — if any exception pattern matches, allow the command
for exception in "${ALLOWED_EXCEPTIONS[@]}"; do
    if echo "$COMMAND" | grep -qE "$exception" 2>/dev/null; then
        exit 0
    fi
done

# Check if first word matches a blocked command
for blocked in "${BLOCKED_CMDS[@]}"; do
    if [ "$FIRST_WORD" = "$blocked" ]; then
        echo "$BLOCK_MESSAGE" >&2
        exit "$EXIT_CODE"
    fi
done

exit 0
