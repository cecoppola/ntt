#!/bin/bash
# mkmanifest.sh - write remote/MANIFEST.sha256: sha256 of every file of the repository copy except the manifest itself.
#   mkmanifest.sh [--check]
# With git: tracked files plus untracked files that are not ignored (so it can run before the commit).  Without git (the target): every
# file below the repository root except .git/ and build/ directories.  Paths are relative to the repository root; s0_check.sh runs
# `sha256sum -c remote/MANIFEST.sha256` from there.  --check: only verify.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); R=$(cd "$HERE/../.." && pwd); M=$R/remote/MANIFEST.sha256
case ${1:-} in -h|--help) sed -n '2,/^HERE/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'; exit 0;; --check) cd "$R" && exec sha256sum -c --quiet "$M";; esac
cd "$R" || exit 2
if git -C "$R" rev-parse --git-dir >/dev/null 2>&1; then L=$(git -C "$R" ls-files -c -o --exclude-standard)
else L=$(find . -type f ! -path './.git/*' ! -path '*/build/*' | sed 's|^\./||'); fi
echo "$L" | grep -v '^remote/MANIFEST.sha256$' | sort -u | while IFS= read -r f; do [ -f "$f" ] && [ ! -L "$f" ] && printf '%s\n' "$f"; done \
  | tr '\n' '\0' | xargs -0 sha256sum > "$M.tmp" && mv "$M.tmp" "$M"
echo "mkmanifest: $(wc -l < "$M") files -> $M"
