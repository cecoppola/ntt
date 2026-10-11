#!/bin/bash
# OWNER=T4
# EST_MIN=6
# T4 (S49) Job B queue item: kit stage nodechk on ALL nodes of the hold (4 hosts, one process each, run alone).
# Run by the Job B driver as: SLURM_JOB_ID=<hold> bash 30_nodechk.sh   (cwd = queue dir).  No rebuild: uses the
# binaries prebuilt in ~/ntt-wt/s49/ecalc and kit_modules.env from ~/batch1011/T4/kit_nc.
# STUB=1 -> kit --dry-run (prints the srun command, runs nothing).
T4=$HOME/batch1011/T4; O=$T4/out/jobB_$(date +%s); mkdir -p "$O"; cp "$T4/kit_nc/kit_modules.env" "$O/"
cd "$HOME/ntt-wt/s49/ecalc" || exit 2
x=; [ "${STUB:-0}" = 1 ] && x=--dry-run
./target_kit.sh --site aac7 --jobid "${SLURM_JOB_ID:?}" --out "$O" --only nodechk $x > "$O/kit.stdout" 2>&1
rc=$?; echo "$rc" > "$O/rc"; cat "$O/KIT_SUMMARY.txt" 2>/dev/null | tail -20; exit $rc
