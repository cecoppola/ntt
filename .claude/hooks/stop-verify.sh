#!/bin/bash
# =============================================================================
# HOOK: stop-verify.sh
# EVENT: Stop
# PURPOSE: Before Claude finishes responding, verify that STATUS.md is current.
#          If source files (.c/.h/.hip) were modified more recently than
#          STATUS.md, Claude is blocked from stopping and instructed to update
#          STATUS.md first. This enforces the "update STATUS.md after every unit"
#          protocol without relying on Claude to remember.
#
# BEHAVIOR: Compares the modification time of STATUS.md against all source files
#          in the project. If any source file is newer, outputs a JSON block
#          decision with a specific reminder. Claude treats this as its next
#          instruction and updates STATUS.md before stopping.
#
#          On the SECOND stop attempt (stop_hook_active = true), the hook always
#          allows stopping — this prevents an infinite blocking loop.
#
# HOW TO ADJUST THIS HOOK:
#   - To DISABLE entirely: set HOOK_ENABLED=0.
#   - To change which file types trigger the check: edit SOURCE_PATTERNS.
#   - To change the reference file checked (default STATUS.md): set REFERENCE_FILE.
#   - To check additional reference files (e.g., also require PLAN.md update):
#     add them to REFERENCE_FILES array.
#   - To allow stop without the check on the FIRST attempt too (effectively
#     disabling loop protection): set RESPECT_STOP_HOOK_ACTIVE=0. NOT recommended.
#   - To add extra directories to exclude from the source file search:
#     add them to EXCLUDE_DIRS.
#   - To change the message Claude receives: edit BLOCK_REASON.
# =============================================================================

# ── TUNABLE VARIABLES ────────────────────────────────────────────────────────

# Master switch. 0 = disabled (always allow stop), 1 = enabled.
HOOK_ENABLED=1

# If true, always allow stop on the second invocation (stop_hook_active=true).
# Setting to 0 disables the infinite-loop guard — do not do this.
# Change to: 1 = safe (recommended), 0 = dangerous (may loop forever)
RESPECT_STOP_HOOK_ACTIVE=1

# The reference file whose modification time is checked against source files.
# If any source file is newer than this file, the hook fires.
# Change to a different file if you want to track a different artifact.
REFERENCE_FILE="STATUS.md"

# File patterns that trigger the freshness check when they are newer than
# REFERENCE_FILE. These are passed to 'find' with -name. Separate with spaces.
# Add patterns for any source file types you want tracked (e.g., "*.py" "*.rs").
SOURCE_PATTERNS=("*.c" "*.h" "*.hip" "*.cu")

# Directories to exclude from the source file search.
# Add directories that contain generated or vendored files you don't own.
EXCLUDE_DIRS=(".git" "build" "dist" "vendor" ".cache")

# Maximum number of newer source files to list in the block message.
# Higher numbers give Claude more context; lower numbers keep the message short.
# Recommended range: 3 (terse) to 10 (detailed).
MAX_FILES_LISTED=5

# The message Claude receives when blocked. It becomes Claude's next instruction.
# Be specific: name what to update and what sections matter.
BLOCK_REASON="Source files were modified more recently than STATUS.md. Update STATUS.md now: fill in Component Status (file, status, notes), API Surface (exact signatures implemented), Deviations from Design Doc, and Next Step. Then stop."

# ── HOOK LOGIC ───────────────────────────────────────────────────────────────

if [ "$HOOK_ENABLED" -eq 0 ]; then
    exit 0
fi

INPUT=$(cat)

# Infinite-loop guard: if Claude already continued once due to this hook,
# allow it to stop unconditionally on the next attempt.
if [ "$RESPECT_STOP_HOOK_ACTIVE" -eq 1 ]; then
    ACTIVE=$(echo "$INPUT" | jq -r '.stop_hook_active // false' 2>/dev/null)
    if [ "$ACTIVE" = "true" ]; then
        exit 0
    fi
fi

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
REF_PATH="$PROJECT_DIR/$REFERENCE_FILE"

# If the reference file doesn't exist yet, no check is possible — allow stop.
if [ ! -f "$REF_PATH" ]; then
    exit 0
fi

# Build the find command with exclusions and source patterns
FIND_ARGS=("$PROJECT_DIR" -newer "$REF_PATH")

# Add directory exclusions
for dir in "${EXCLUDE_DIRS[@]}"; do
    FIND_ARGS+=(-not -path "*/$dir/*")
done

# Add file pattern conditions (OR logic between patterns)
FIND_ARGS+=("(")
first=1
for pattern in "${SOURCE_PATTERNS[@]}"; do
    if [ "$first" -eq 1 ]; then
        first=0
    else
        FIND_ARGS+=(-o)
    fi
    FIND_ARGS+=(-name "$pattern")
done
FIND_ARGS+=(")")

NEWER_FILES=$(find "${FIND_ARGS[@]}" 2>/dev/null | head -"$MAX_FILES_LISTED")

if [ -n "$NEWER_FILES" ]; then
    # Format the file list for the message
    FILE_LIST=$(echo "$NEWER_FILES" | sed "s|$PROJECT_DIR/||g" | tr '\n' ' ' | sed 's/ $//')
    FULL_REASON="Modified source files newer than $REFERENCE_FILE: [$FILE_LIST]. $BLOCK_REASON"

    # Output JSON block decision. Claude reads 'reason' as its next instruction.
    printf '{"decision": "block", "reason": "%s"}' \
        "$(echo "$FULL_REASON" | sed 's/"/\\"/g')"
fi

exit 0
