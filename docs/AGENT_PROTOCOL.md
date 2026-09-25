# AGENT_PROTOCOL.md — the common protocol for work agents

Kept in the repository (the session scratchpad is cleared between sessions). The integrator names each agent
(`<AGENT>`), its branch base and its files in the agent's prompt; everything else is here.

## Repository

- Work in an isolated git worktree of `/home/machinus/apucode/ntt`. Create your branch `p<phase>-<AGENT>` from the
  base the prompt names.
- Read first: `PLAN.md` (the current phase's section; your row), `TASKS.md`'s front section, the last `RESULTS.md`
  sections, `ecalc/README.md` (the switch table), `docs/TARGET.md` / `docs/TARGET_TASKS.md` if the target is involved,
  and the files you own.
- **Digits must stay byte-identical.** Every new behavior goes behind an environment switch, **off by default**; the
  user decides adoption. One row per new switch in `ecalc/README.md`.
- Own only the files the prompt lists; a necessary edit elsewhere is minimal, commented, and named in your report.
- Commit in small steps; messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not edit `PLAN.md` or `RESULTS.md`. Write `results/<AGENT><phase>.md`: design, every test with command, size,
  result and time, open issues, and a **RESUME** section.
- **Commit and update RESUME after every step.** The account's usage limit can stop you without warning; the
  integrator resumes you (or a successor) from RESUME.
- Files under `results/` other than `.md` need `git add -f`. Label every number **measured / modelled / assumed**.

## aac6

- Access: `sshpass -p emilewoods ssh -o ConnectTimeout=20 chcoppola@aac6.amd.com` — **direct, no ProxyJump, no ssh
  config host**; `sshpass -p emilewoods scp -o ConnectTimeout=20 <file> chcoppola@aac6.amd.com:~/`.
- The aac6 clones cannot pull from GitHub. Your clone: `~/ntt-<AGENT><phase>` (`git clone ~/ntt …`; never touch `~/ntt`
  or another agent's clone). Deliver by bundle **from a commit the clone has** (check `git -C ~/ntt log --oneline -1`
  on aac6 first); after fetching, **check `git log --oneline -1` shows your commit**.
- Build: `bash -lc "module load rocm; cd ~/ntt-<AGENT><phase>/ecalc && make -s -j16"` (plain ssh has no hipcc; the
  login node builds and runs python, `BS_LAYOUT_ONLY` and `MN_PLAN_ONLY`).
- Do not leave untracked files in `~/ntt` (they block the integrator's pulls).

## Nodes (PPAC_MI300A_SPX: s24-16, s24-26, s24-30; s24-35 down)

- One job at a time, ≤ 45 min: `sbatch -p PPAC_MI300A_SPX -N1 [-w <node>] --gpus=4 -t 0:45:00 -J <AGENT> --parsable
  --wrap "sleep 2700"`; run everything with `srun --jobid=$J …` (multi-process: `SLURM_JOB_ID=$J ./mnrun.sh <p> <cmd>`).
- **Respect nodes the integrator holds** (the prompt names them); use `-w` or `--begin` to stay off them.
- Script each batch to **run unattended**: `setsid nohup`, wait for its own job, cancel itself at the end. Never leave a
  job pending when you stop.
- Evict the reference from the page cache before a timed run (`posix_fadvise DONTNEED`), or use `ECALC_ODIRECT=1`.
- Kill processes only by PID or an anchored `pgrep -f "^bash /shared/…"` pattern; `pkill -f <name>` also matches the
  ssh command that carries it.
- The admin's nightly CI array (`cdash-nightly`) can hold every node from ≈ 00:00 to ≈ 03:30 EDT at higher priority.

## Gates for a code change

`t_newton`, `t_mul`; e9 identical in both bases; 4 × 10¹⁰ identical to `~/ntt/ecalc/results/e_4e10.out`;
`./mnaccept.sh $J --only unit,e9` plus the touched steps (`full` and `stress` also need `--full` / `--stress`);
`mem_model.py --check-c` exact if sizing changed.

## Reporting

Do not end your turn while a job of yours runs (wait with foreground loops of ≤ 10 min per call, or an armed watcher).
Report at the end: branch, commits, what passed with exact commands, what did not, open issues, anything outside your
files.
