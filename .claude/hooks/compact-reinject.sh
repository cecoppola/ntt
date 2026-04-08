#!/bin/bash
# =============================================================================
# HOOK: compact-reinject.sh
# EVENT: SessionStart (matcher: "compact")
# PURPOSE: After Claude Code auto-compacts the context window, conversation
#          history is summarized and details are lost. This hook re-injects
#          critical project state so Claude always has it after a compaction,
#          without needing you to re-explain the current task.
#
#          Anything this script writes to stdout is added to Claude's context
#          immediately after compaction completes.
#
# BEHAVIOR: Reads STATUS.md and injects it as a reminder. Also prints a short
#          protocol reminder so Claude knows what to do next.
#
# HOW TO ADJUST THIS HOOK:
#   - To DISABLE re-injection: set HOOK_ENABLED=0.
#   - To inject additional files: add more "echo" or "cat" calls in the
#     INJECTED CONTENT section below.
#   - To change what is injected: edit the INJECT_FILES array and/or
#     the REMINDER_TEXT variable.
#   - To suppress the protocol reminder: set INJECT_REMINDER=0.
#   - To inject git log for recent context: set INJECT_GIT_LOG=1 and
#     adjust GIT_LOG_LINES to control how many commits are shown.
# =============================================================================

# ── TUNABLE VARIABLES ────────────────────────────────────────────────────────

# Master switch. 0 = disabled (inject nothing), 1 = enabled.
HOOK_ENABLED=1

# Files to re-inject after compaction. Paths are relative to the project root.
# Each file's contents are printed to stdout and become part of Claude's context.
# Add or remove files to control what survives compaction.
INJECT_FILES=(
    "STATUS.md"        # Project state navigator — always re-inject this
    # "PLAN.md"        # Uncomment to also re-inject the design plan
    # "RULES.md"       # Uncomment to re-inject project rules explicitly
)

# Whether to inject a short protocol reminder after the files.
# 0 = no reminder, 1 = inject the REMINDER_TEXT below.
INJECT_REMINDER=1

# The reminder text injected after file contents. This tells Claude what to
# do next after a compaction. Customize this to match your workflow.
REMINDER_TEXT="[POST-COMPACT REMINDER] Context was just compacted. Re-read STATUS.md above. Continue with the unit listed under 'Next Step'. Do not start a new unit without completing and recording the current one. Use Grep/Read/Glob tools — not Bash grep/cat/find."

# Whether to inject a short git log for recent commit context.
# 0 = disabled, 1 = enabled. Useful when you want Claude to know recent changes.
INJECT_GIT_LOG=0

# Number of recent git commits to show if INJECT_GIT_LOG=1.
# Higher numbers give more context but consume more tokens.
# Recommended range: 3 (minimal) to 10 (detailed recent history).
GIT_LOG_LINES=5

# ── HOOK LOGIC ───────────────────────────────────────────────────────────────

if [ "$HOOK_ENABLED" -eq 0 ]; then
    exit 0
fi

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"

echo "=== POST-COMPACT CONTEXT RE-INJECTION ==="
echo ""

for file in "${INJECT_FILES[@]}"; do
    filepath="$PROJECT_DIR/$file"
    if [ -f "$filepath" ]; then
        echo "--- $file ---"
        cat "$filepath"
        echo ""
    fi
done

if [ "$INJECT_GIT_LOG" -eq 1 ]; then
    if git -C "$PROJECT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        echo "--- Recent commits ---"
        git -C "$PROJECT_DIR" log --oneline -"$GIT_LOG_LINES" 2>/dev/null
        echo ""
    fi
fi

if [ "$INJECT_REMINDER" -eq 1 ]; then
    echo "$REMINDER_TEXT"
fi

exit 0
