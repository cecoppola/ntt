#!/bin/bash
# tools/profile_check.sh - READ-ONLY comparison of a machine profile (profiles/*.conf) with what the launch scripts set today.
#
# USAGE
#   tools/profile_check.sh <profile.conf> [-q] [--strict]
#     -q        only the problems (MISMATCH, UNKNOWN, UNFILLED, REMOVAL-SLATED, SCRIPT-ONLY), no MATCH / PROFILE-ONLY lines
#     --launch=aac7|none  compare with the aac7 launch lines (default: only for aac7*.conf)
#     --strict  exit 1 if any MISMATCH or UNKNOWN is found (default exit 0: it reports, it does not gate)
#
# WHAT IT DOES (nothing is launched, no cluster is touched, no file is written outside a mktemp dir that is removed)
#   1. Sources the profile in an `env -i` subshell and reads the value of every KEY=value line (so $HOME and quoting are expanded).
#   2. Collects what the scripts set:
#        e16     the words of LINE= in ecalc/e16_headline.sh (parsed the way tools/gate.sh does)
#        aac7env the variables ecalc/aac7env.sh exports when sourced (module is stubbed; the /opt/cray/pe/sma test is forced true)
#        default the literal ${NAME:-value} / ${NAME-value} defaults written in mnrun.sh, aac7env.sh, target_kit.sh, e16_headline.sh
#                (values containing $ are computed there: reported as COMPUTED, never compared)
#   3. Per profile key: MATCH (the launch lines e16 / aac7env set it to the same value), MISMATCH (they set a different one),
#      PROFILE-ONLY (no launch line sets it; script defaults are shown for information: a profile may legitimately override them),
#      UNFILLED (value __FILL__).
#   4. UNKNOWN: a profile key whose name appears nowhere in ecalc/*.c|*.h|*.sh, tools/*, Makefiles (external library variables
#      FI_* SHMEM_* XT_* OMPI_* SLURM_* are labelled EXTERNAL instead).
#   5. SCRIPT-ONLY: keys that e16 LINE or aac7env.sh set but the profile lacks.
#   6. REMOVAL-SLATED: profile keys named in the tier A / B tables of results/A11.md (code reduction).
# Run it anywhere (login node, laptop): it needs bash and grep only.  On a machine without /opt/cray/pe/sma the aac7env module
# variables are still reported (the test is forced).
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(cd "$HERE/.." && pwd); E=$ROOT/ecalc
PROF=; Q=0; STRICT=0; LAUNCH=auto
for a in "$@"; do case $a in -q) Q=1;; --strict) STRICT=1;; --launch=*) LAUNCH=${a#*=};; -h|--help) sed -n '2,25p' "$0"; exit 0;; *) PROF=$a;; esac; done
[ -r "${PROF:-}" ] || { echo "usage: profile_check.sh <profile.conf> [-q] [--strict]" >&2; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/profcheck.XXXXXX"); trap 'rm -rf "$T"' EXIT

# ---- 1. the profile: keys (in file order) and effective values
grep -oE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*=' "$PROF" | tr -d ' \t=' | awk '!s[$0]++' > "$T/keys"
env -i HOME="$HOME" PATH="$PATH" bash -c '. "$1"; while read -r k; do printf "%s\t%s\n" "$k" "${!k-}"; done < "$2"' _ "$PROF" "$T/keys" > "$T/prof" 2> "$T/prof.err" \
    || { echo "profile_check: cannot source $PROF:"; cat "$T/prof.err"; exit 2; }

# ---- 2. what the scripts set.  Format of $T/src: key <TAB> source <TAB> kind(set|default|computed) <TAB> value
: > "$T/src"
# e16: LINE words
if [ -r "$E/e16_headline.sh" ]; then
    LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ "$E/e16_headline.sh" 2>/dev/null)
    for w in $LINE; do case $w in *=*) printf '%s\te16\tset\t%s\n' "${w%%=*}" "${w#*=}";; esac; done >> "$T/src"
fi
# aac7env: exported variables after sourcing (module stubbed, sma directory test forced)
if [ -r "$E/aac7env.sh" ]; then
    sed 's#/opt/cray/pe/sma#/#' "$E/aac7env.sh" > "$T/aac7env_forced.sh"
    env -i HOME="$HOME" PATH="$PATH" bash -c 'module() { :; }; env | sort > "$2.before"; . "$1"; env | sort > "$2.after"' _ "$T/aac7env_forced.sh" "$T/ae" 2>/dev/null
    comm -13 "$T/ae.before" "$T/ae.after" | while IFS= read -r l; do k=${l%%=*}; printf '%s\taac7env\tset\t%s\n' "$k" "${l#*=}"; done >> "$T/src"
fi
# literal defaults in the scripts
for f in mnrun.sh aac7env.sh target_kit.sh e16_headline.sh; do
    [ -r "$E/$f" ] || continue
    grep -oE '\$\{[A-Z][A-Z0-9_]*:?-[^}]*\}' "$E/$f" | sort -u | while IFS= read -r m; do
        m=${m#\$\{}; m=${m%\}}; k=${m%%[:-]*}; v=${m#"$k"}; v=${v#:}; v=${v#-}
        case $v in *'$'*) kind=computed;; *) kind=default;; esac
        printf '%s\t%s\t%s\t%s\n' "$k" "${f%.sh}" "$kind" "$v"
    done >> "$T/src"
done

# the e16 / aac7env launch lines are aac7's: compare against them only for an aac7 profile (--launch=aac7 forces, --launch=none drops)
[ "$LAUNCH" = auto ] && { case $(basename "$PROF") in aac7*) LAUNCH=aac7;; *) LAUNCH=none;; esac; }
if [ "$LAUNCH" != aac7 ]; then awk -F'\t' '$3 != "set"' "$T/src" > "$T/src.2"; mv "$T/src.2" "$T/src"; fi

# ---- code-known names
grep -rhoE '\b[A-Z][A-Z0-9_]{2,}\b' "$E"/*.c "$E"/*.h "$E"/*.sh "$E"/Makefile "$ROOT"/tools/* 2>/dev/null | sort -u > "$T/known"

# ---- removal-slated (A11 tier A / B)
: > "$T/slated"
[ -r "$ROOT/results/A11.md" ] && awk '/^### Tier [AB]/ { on = 1; next } /^### Tier C/ { on = 0 } on && /^\| [AB][0-9]+ \|/ { print }' "$ROOT/results/A11.md" \
    | cut -d'|' -f2,4 | grep -oE '[A-B][0-9]+ *\||`[A-Z][A-Z0-9_]*`' | awk '/\|/ { id = $1; next } { gsub("`", ""); print $0 "\t" id }' > "$T/slated"

# ---- 3-6. report
nmis=0 nunk=0 nfill=0 nslate=0 nmatch=0 nonly=0
printf 'profile_check: %s  (scripts under %s)\n' "$PROF" "$E"
while IFS=$'\t' read -r k v; do
    if [ "$v" = __FILL__ ]; then
        nfill=$((nfill + 1)); echo "UNFILLED      $k"
        if ! grep -qx "$k" "$T/known"; then case $k in FI_*|SHMEM_*|XT_*|OMPI_*|SLURM_*) ;; *) nunk=$((nunk + 1)); echo "UNKNOWN       $k : no consumer in ecalc/ or tools/ (profile-only meta key: needs the entry point to read it)";; esac; fi
        continue
    fi
    vals=; bad=0; n=0; dv=; dsame=0; dn=0
    while IFS=$'\t' read -r sk ss kind sv; do
        [ "$sk" = "$k" ] || continue
        case $kind in
            set)      n=$((n + 1)); vals="$vals $ss=$sv"; [ "$sv" = "$v" ] || bad=1;;
            default)  [ -n "$sv" ] && [ "$sv" != "<unset>" ] && [ "$sv" != "<system>" ] || continue
                      dn=$((dn + 1)); dv="$dv $ss=$sv"; [ "$sv" = "$v" ] && dsame=$((dsame + 1));;
            computed) dv="$dv $ss=COMPUTED";;
        esac
    done < <(sort -u "$T/src")
    ext=
    if ! grep -qx "$k" "$T/known"; then
        case $k in FI_*|SHMEM_*|XT_*|OMPI_*|SLURM_*) ext=" [EXTERNAL library variable]";;
                   *) nunk=$((nunk + 1)); echo "UNKNOWN       $k=$v : no such name in ecalc/ or tools/ (new key without a consumer, or a typo)";; esac
    fi
    sl=$(awk -F'\t' -v k="$k" '$1 == k { print $2; exit }' "$T/slated"); [ -n "$sl" ] && { nslate=$((nslate + 1)); echo "REMOVAL-SLATED $k : tier item $sl of results/A11.md"; }
    if [ "$bad" = 1 ]; then nmis=$((nmis + 1)); echo "MISMATCH      $k profile=$v ; launch script sets:$vals"
    elif [ "$n" -gt 0 ]; then nmatch=$((nmatch + 1)); [ "$Q" = 1 ] || echo "MATCH         $k=$v ; launch script sets:$vals$ext"
    else
        nonly=$((nonly + 1))
        if [ "$Q" != 1 ]; then
            note=; [ -n "$dv" ] && { if [ "$dn" -gt 0 ] && [ "$dsame" = "$dn" ]; then note=" ; equals the script default:$dv"; else note=" ; script defaults (the profile overrides):$dv"; fi; }
            echo "PROFILE-ONLY  $k=$v$note$ext"
        fi
    fi
done < "$T/prof"
# keys the launch scripts set that the profile lacks (e16 LINE, aac7env exports)
awk -F'\t' '$3 == "set" && ($2 == "e16" || $2 == "aac7env") { print $1 "\t" $2 "\t" $4 }' "$T/src" | sort -u | while IFS=$'\t' read -r k s v; do
    grep -qx "$k" "$T/keys" || printf 'SCRIPT-ONLY   %s=%s  (set by %s, not in the profile)\n' "$k" "$v" "$s"
done > "$T/so"
nso=$(wc -l < "$T/so"); cat "$T/so"
printf 'summary: %d keys; MATCH %d, MISMATCH %d, PROFILE-ONLY %d, UNFILLED %d, UNKNOWN %d, REMOVAL-SLATED %d, SCRIPT-ONLY %d\n' \
    "$(wc -l < "$T/prof")" "$nmatch" "$nmis" "$nonly" "$nfill" "$nunk" "$nslate" "$nso"
[ "$STRICT" = 1 ] && [ $((nmis + nunk)) -gt 0 ] && exit 1
exit 0
