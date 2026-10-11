#!/bin/bash
# T4 (S49) aac6: nodechk body run INSIDE a Slurm job on one PPAC node.  usage: aac6_job.sh <label>
# Kit stage nodechk with a hand-written kit_modules.env (aac6 has no Cray SHMEM, so the kit's build stage cannot run);
# if the kit path yields no NODECHK line, fall back to the raw program via srun.
L=${1:?label}; T4=$HOME/batch1011/T4; O=$T4/out/$L; mkdir -p "$O"; cp "$T4/kit_modules.env" "$O/"
cd "$HOME/ntt-s49/ecalc" || exit 2
./target_kit.sh --site aac7 --jobid "$SLURM_JOB_ID" --out "$O" --only nodechk > "$O/kit.stdout" 2>&1; rc=$?
if ! grep -aq '^NODECHK .*apu0_s=[0-9]' "$O/logs/06_nodechk.log" 2>/dev/null; then
  echo "kit gave no NODECHK (rc=$rc); fallback direct srun" > "$O/fallback"
  mkdir -p "$O/logs"; srun --jobid="$SLURM_JOB_ID" -N1 -n1 --gpus=4 --overlap bash -lc 'module load rocm >/dev/null 2>&1; exec "$@"' _ "$HOME/ntt-s49/ecalc/tests/t_nodechk" 1 3 >> "$O/logs/06_nodechk.log" 2>&1; rc=$?
fi
echo $rc > "$O/rc"; exit $rc
