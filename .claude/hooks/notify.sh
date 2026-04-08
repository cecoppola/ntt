#!/bin/bash
# =============================================================================
# HOOK: notify.sh
# EVENT: Notification
# PURPOSE: Surfaces Claude Code notifications as desktop notifications using
#          notify-send (libnotify). Useful when Claude is running a long task
#          in the background and you want an OS-level alert when it needs input
#          or when it finishes.
#
# BEHAVIOR: Reads the notification message from the JSON input and calls
#          notify-send to pop a desktop notification. Falls back to a bell
#          character on stdout if notify-send is not installed.
#
# HOW TO ADJUST THIS HOOK:
#   - To DISABLE entirely: set HOOK_ENABLED=0.
#   - To change the notification title: edit NOTIFY_TITLE.
#   - To change urgency level: edit NOTIFY_URGENCY (low, normal, critical).
#   - To change how long the notification stays visible: edit NOTIFY_TIMEOUT_MS.
#     Set to -1 to keep it visible until dismissed.
#   - To suppress the fallback bell when notify-send is absent: set FALLBACK_BELL=0.
#   - To also log notifications to a file: set LOG_ENABLED=1 and LOG_FILE path.
#   - To add an icon: set NOTIFY_ICON to an icon name (e.g., "dialog-information")
#     or an absolute path to a PNG file.
# =============================================================================

# ── TUNABLE VARIABLES ────────────────────────────────────────────────────────

# Master switch. 0 = disabled (no desktop notifications), 1 = enabled.
HOOK_ENABLED=1

# Title shown in the desktop notification popup.
# Change to any string — project name, "Claude Code", etc.
NOTIFY_TITLE="Claude Code"

# Urgency level passed to notify-send. Controls visual priority.
# Values: low (subdued), normal (standard), critical (persistent/red on some DEs)
NOTIFY_URGENCY="normal"

# How long the notification stays on screen, in milliseconds.
# Set to -1 to require manual dismissal. Recommended: 8000 (8 seconds).
NOTIFY_TIMEOUT_MS=8000

# Icon for the notification. Can be a named icon from the system theme
# (e.g., "dialog-information", "dialog-warning", "terminal") or an absolute
# path to a PNG file. Leave empty to use the default icon.
NOTIFY_ICON="dialog-information"

# If notify-send is not found, emit a terminal bell character as fallback.
# 0 = silent fallback (do nothing), 1 = emit bell character.
FALLBACK_BELL=1

# Enable logging notifications to a file. Useful for reviewing what Claude
# reported while you were away. 0 = disabled, 1 = enabled.
LOG_ENABLED=0

# Path to the log file when LOG_ENABLED=1. Relative to CLAUDE_PROJECT_DIR.
LOG_FILE=".claude/notifications.log"

# ── HOOK LOGIC ───────────────────────────────────────────────────────────────

if [ "$HOOK_ENABLED" -eq 0 ]; then
    exit 0
fi

INPUT=$(cat)

# Extract the notification message from the JSON payload.
# The Notification event provides a 'message' field.
MESSAGE=$(echo "$INPUT" | jq -r '.message // empty' 2>/dev/null)

if [ -z "$MESSAGE" ]; then
    exit 0
fi

# Log to file if enabled
if [ "$LOG_ENABLED" -eq 1 ]; then
    PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
    LOG_PATH="$PROJECT_DIR/$LOG_FILE"
    TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$TIMESTAMP] $MESSAGE" >> "$LOG_PATH" 2>/dev/null
fi

# Send desktop notification
if command -v notify-send >/dev/null 2>&1; then
    ICON_ARG=""
    if [ -n "$NOTIFY_ICON" ]; then
        ICON_ARG="--icon=$NOTIFY_ICON"
    fi
    notify-send \
        --urgency="$NOTIFY_URGENCY" \
        --expire-time="$NOTIFY_TIMEOUT_MS" \
        $ICON_ARG \
        "$NOTIFY_TITLE" \
        "$MESSAGE" 2>/dev/null
else
    # notify-send not available — fall back to terminal bell
    if [ "$FALLBACK_BELL" -eq 1 ]; then
        printf '\a'
    fi
fi

exit 0
