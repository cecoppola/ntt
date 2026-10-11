#!/bin/bash
# T4 (S49): one-node nodechk run body, executed INSIDE a Slurm allocation (sbatch --wrap target) on aac7.
# usage: nc_job.sh <label>   -> ~/batch1011/T4/out/<label>/ (KIT_SUMMARY.txt, logs/)
# Uses the prebuilt kit in ~/ntt-wt/s49/ecalc (binaries built once, never rebuilt here) and a copy of kit_modules.env.
L=${1:?label}; T4=$HOME/batch1011/T4; O=$T4/out/$L
mkdir -p "$O"; cp "$T4/kit_nc/kit_modules.env" "$O/"
cd "$HOME/ntt-wt/s49/ecalc" || exit 2
./target_kit.sh --site aac7 --jobid "$SLURM_JOB_ID" --out "$O" --only nodechk > "$O/kit.stdout" 2>&1
rc=$?; echo "$rc" > "$O/rc"; exit $rc
