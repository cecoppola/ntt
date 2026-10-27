#!/bin/bash
# show.sh - bounded extract (<= 80 lines) of a run directory or a file.
#   show.sh <run dir|stage|latest|file> [errors|tail N|grep PATTERN|phases|env|files]
# <stage> is s0..s5, alloc, chain (or a name like s3_gate): the newest run of that stage; latest = the newest run of any stage.
# no mode: the summary block of the run.   errors: up to 40 distinct error lines of the logs.   tail N: last N lines of the newest raw log
# (N <= 80).   grep PATTERN: matching lines of all logs (extended regex, file name in front).   phases: RESULT/phase lines of the ecalc logs.
# env: site.env and the launch settings mnrun.sh printed (FI_*, SHMEM_*, XSHMEM_*, pool).   files: file list of the run.
# On a file: tail N / grep PATTERN / errors / head N (default tail 60).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/lib.sh"
T=${1:-}; [ -n "$T" ] && [ "$T" != -h ] && [ "$T" != --help ] || { rl_help "$0"; exit 0; }
shift; MODE=${1:-summary}; ARG=${2:-}
cap() { head -n 80; }
show_main() {
  D=
  if [ -d "$T" ]; then D=$T
  elif [ -f "$T" ]; then F=$T
  elif [ "$T" = latest ]; then D=$(readlink -f "$REMOTE_OUT/latest" 2>/dev/null)
  else s=${T%/}; s=${s%.sh}; case $s in s[0-7]_*) s=${s%%_*};; esac; D=$(readlink -f "$REMOTE_OUT/${s}_latest" 2>/dev/null); [ -d "$D" ] || D=$(ls -dt "$REMOTE_OUT/${s}"_2* "$REMOTE_OUT/${s}"*_2* 2>/dev/null | head -1); fi
  if [ -n "${F:-}" ]; then
      case $MODE in
        summary|tail) n=${ARG:-60}; [ "$n" -gt 80 ] && n=80; rl_cat "$F" | tail -n "$n";;
        head) n=${ARG:-60}; [ "$n" -gt 80 ] && n=80; rl_cat "$F" | head -n "$n";;
        grep) rl_cat "$F" | grep -a -E -- "${ARG:?pattern}" | cut -c1-240 | cap;;
        errors) rl_cat "$F" | grep -a -i -E 'error|fail|abort|segv|refused|timeout|fatal' | grep -a -v -E '^ *([0-9]+: )?config: |SEGV_TRACE' | cut -c1-200 | head -n 40;;
        *) echo "show.sh: mode for a file: tail N | head N | grep PATTERN | errors" >&2; exit 2;;
      esac; exit 0
  fi
  [ -d "$D" ] || { echo "show.sh: nothing found for '$T' under $REMOTE_OUT" >&2; exit 2; }
  logs() { ls -t "$D"/raw/*.log "$D"/raw/*.log.gz "$D"/raw/*.out "$D"/raw/*.txt "$D"/*.log "$D"/*/raw/*.log "$D"/*/build.out 2>/dev/null; }
  case $MODE in
    summary) [ -f "$D/summary.txt" ] && cap < "$D/summary.txt" || { echo "(no summary yet) $D"; tail -n 20 "$D/log.txt" 2>/dev/null; };;
    errors) { rl_cat $(logs) "$D/log.txt"; } | grep -a -i -E 'error|fail|abort|segv|refused|timeout|fatal' | grep -a -v -i -E 'fail(ed)?=0|errors?[:=] *0|without error|VERIFY OK|no error|^ *([0-9]+: )?config: |SEGV_TRACE' \
          | sed -E 's/^ *[0-9]+: //' | cut -c1-200 | awk '{ k = $0; gsub(/[0-9]+/, "N", k); if (!(k in s)) { s[k] = 1; print } }' | head -n 40;;
    tail) n=${ARG:-40}; [ "$n" -gt 80 ] && n=80; f=$(logs | head -1); [ -n "$f" ] || f=$D/log.txt; echo "== $f"; rl_cat "$f" | tail -n "$n" | cut -c1-240;;
    grep) for f in $(logs) "$D/log.txt"; do rl_cat "$f" | grep -a -E -- "${ARG:?pattern}" | cut -c1-240 | sed "s|^|$(basename "$f"): |"; done | cap;;
    phases) for f in $(logs); do rl_cat "$f" | sed -E 's/^ *[0-9]+: //' | grep -a -E '^RESULT ecalc|^(init|bs|dm|dc|recip|T1|T2|total|other) +[0-9.]+ s|VERIFY|^mn: all' | sed "s|^|$(basename "$f"): |"; done | cut -c1-200 | cap;;
    env) { echo "== site.env"; grep -v -i -E 'pass|token|key' "$SITE_ENV" 2>/dev/null; echo "== launch (mnrun.sh lines)"; for f in $(logs); do rl_cat "$f" | grep -a -E '^mnrun.sh: ' ; done | sort -u; } | cut -c1-220 | cap;;
    files) (cd "$D" && find . -maxdepth 3 -type f -printf '%s %p\n' | sort -k2 | head -n 80);;
    *) echo "show.sh: mode errors | tail N | grep PATTERN | phases | env | files" >&2; exit 2;;
  esac
}
show_main | rl_tilde; exit ${PIPESTATUS[0]}   # the value of $HOME is shown as ~
