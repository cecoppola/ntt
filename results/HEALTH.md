# HEALTH — node health gate (C4) and per-APU memory guard / high-water (C5)
**Question.** (C4) Can the kit tell a node with uncorrectable memory errors, a reboot, or NIC errors during our run from a healthy one? (C5) What does one APU really need (not the pool's reserved footprint), and can a guard watch each APU's own memory?
**Change / branch.** Branch `health`. Switches: `ECALC_MEM_GUARD_APU_GB` (default 0 = off), `ECALC_MEMSAMPLE_SYSFS_ROOT` (test hook). The per-APU high-water rides the memory sampler (`ECALC_MEM_SAMPLE` or a guard turns it on; no new switch).
**Method.** (filled in below)
**Results.** (filled in below)
**Verdict.** (filled in below)
**Open issues.** (filled in below)
**RESUME.** branch `health`, C4 committed; C5 code written, live tests pending.
