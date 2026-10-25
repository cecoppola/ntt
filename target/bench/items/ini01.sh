#!/bin/bash
# ini01 (TQ08) - is the libfabric userfaultfd memory monitor usable (kit stage uffd): the userfaultfd syscall on a compute node, then a tiny
# multi-process ecalc (1 node, 4 PEs) with FI_MR_CACHE_MONITOR=userfaultfd and FI_LOG_LEVEL=info: look for "memory monitor is: uffd" and a clean run.
. "$(dirname "${BASH_SOURCE[0]}")/_ecalc.sh"
ecalc_have 1 || ecalc_have 2 || { tgt_kv note "no binary"; ecalc_end SKIP; }
PN=1; ecalc_have 1 || PN=2
cat > raw/uffd.py <<'PY'
import ctypes, platform, os
nr = {'x86_64': 323, 'aarch64': 282}.get(platform.machine())
libc = ctypes.CDLL(None, use_errno=True)
if nr is None: print('UFFD syscall unknown arch', platform.machine())
else:
    fd = libc.syscall(nr, os.O_CLOEXEC | os.O_NONBLOCK)
    print('UFFD syscall fd=%d errno=%d' % (fd, ctypes.get_errno()))
PY
ecalc_srun uffd_sys $PN 1 python3 raw/uffd.py
SYS=$(grep -a -m1 '^UFFD syscall' raw/uffd_sys.log); echo "$SYS"
tgt_kv e.uffd_syscall "${SYS:-no output}"
tgt_kv e.vm_unprivileged_userfaultfd "$(cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null || echo NA)"
S=P; [ $PN = 2 ] && S=C
N=2; [ "$(echo "$HOSTS" | tr ',' '\n' | grep -c .)" -ge 2 ] || N=1
ecalc_run uffd_info $S $N "$(ecalc_digits 1e9)" FI_MR_CACHE_MONITOR=userfaultfd FI_LOG_LEVEL=info; ecalc_kv uffd_info
MON=$(ecalc_grab "$E_LOG" 'memory monitor is' | cut -c1-120)
ecalc_run uffd_off $S $N "$(ecalc_digits 1e9)" FI_MR_CACHE_MONITOR=disabled; ecalc_kv uffd_off
tgt_kv e.monitor_line "${MON:-none}"
GOT=no; echo "$MON" | grep -q 'uffd' && GOT=yes
ecalc_q TQ08 "userfaultfd syscall: ${SYS:-none}; libfabric 'memory monitor' line: ${MON:-not seen}; ecalc 1e9 with monitor=userfaultfd: $(ecalc_load uffd_info; ecalc_brief uffd_info); with monitor disabled: $(ecalc_load uffd_off; ecalc_brief uffd_off)"
case "$SYS" in *"fd=-1"*|"") ecalc_end PARTIAL;; esac
[ "$GOT" = yes ] && ecalc_end PASS || ecalc_end PARTIAL
