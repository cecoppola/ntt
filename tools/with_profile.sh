#!/bin/bash
# tools/with_profile.sh PROFILE [--] CMD...   run CMD with the machine profile loaded (precedence: explicit env > profile > code default).
#   PROFILE = NAME (profiles/NAME.conf) or a path.  Validates first (unknown/misspelled key, __FILL__ = error, nothing runs).
#   Prints one provenance line before CMD:  config-src: profile=NAME from-profile=K,K env-over=K,K (code defaults: all other switches)
#   `from-profile` = values the profile supplied; `env-over` = profile keys the caller's environment overrode.
#   Without this wrapper nothing reads a profile and behaviour is unchanged.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ $# -ge 2 ] || { sed -n '2,6p' "$0" | cut -c3-; exit 2; }
P=$1; shift; [ "$1" = -- ] && shift
OUT=$(python3 "$HERE/profile_load.py" "$P") || exit 1
eval "$OUT"
echo "config-src: profile=$ECALC_PROFILE_NAME from-profile=${ECALC_PROFILE_FROM:--} env-over=${ECALC_PROFILE_OVER:--} (code defaults: all other switches)"
exec "$@"
