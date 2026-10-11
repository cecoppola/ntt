#!/usr/bin/env python3
"""envprobe_offline.py - C04: rebuild ENVPROBE sidecars from A13's data (probe logs + NIC snapshots) so calib_table.py can test whether the
probe predicts the run.   usage: envprobe_offline.py A13DIR OUTDIR     (A13DIR holds A/log/*.plain, A/probes.tsv, nic/<tag>_pre|post.<host>.txt)
Run k = rNNN is bracketed by the probes p(k-1) (before) and p(k) (after).  Writes OUTDIR/log/rNNN.log (a copy of the run log) and rNNN.probe
(two ENVPROBE lines).  The NIC group/fgfc of a tag = counter deltas of its pre/post snapshots (same rule as tools/envprobe.sh)."""
import glob, os, re, shutil, sys

def snap(path):
    a = b = f = 0.0
    for ln in open(path, errors='replace'):
        m = re.match(r'cxi\d/oxe_mcu_meas_(\d+) (\d+)@', ln)
        if m:
            if int(m.group(1)) < 26: a += int(m.group(2))
            else: b += int(m.group(2))
            continue
        m = re.match(r'cxi\d/oxe_stall_fgfc_start_0 (\d+)@', ln)
        if m: f += int(m.group(1))
    return a, b, f

tag_pre = {}   # tag -> tag of the snapshot taken just before it (filled by main)

def tag_group(nic, tag):
    dA = dB = dF = 0.0; n = 0
    # snapped p000 pre+post, but for later probes only the post: the pre of probe pK is the post of run rK (run K precedes probe K... see main)
    pre0 = tag_pre.get(tag, tag + '_pre')
    for pre in glob.glob(os.path.join(nic, pre0 + '.*.txt')):
        post = os.path.join(nic, tag + '_post.' + pre.split('.')[-2] + '.txt')
        if not os.path.exists(post): continue
        a0, b0, f0 = snap(pre); a1, b1, f1 = snap(post); dA += a1 - a0; dB += b1 - b0; dF += f1 - f0; n += 1
    return ('NA', None, n) if n == 0 or dA + dB <= 0 else ('A' if dA > dB else 'B', dF / 1e6, n)

def main(d, out):
    probes = {}
    for ln in list(open(os.path.join(d, 'A', 'probes.tsv')))[1:]:
        c = ln.split('\t'); probes[c[0]] = (float(c[3]), float(c[2]))      # label -> (r64, r256)
    os.makedirs(os.path.join(out, 'log'), exist_ok=True)
    nic = os.path.join(d, 'nic')
    for lg in sorted(glob.glob(os.path.join(d, 'A', 'log', 'r*.plain'))):
        k0 = int(os.path.basename(lg)[1:4]); tag_pre['p%03d' % k0] = 'r%03d_post' % k0   # probe pK follows run rK
        lab = os.path.basename(lg)[:-6]; k = int(lab[1:])
        shutil.copy(lg, os.path.join(out, 'log', lab + '.log'))
        with open(os.path.join(out, 'log', lab + '.probe'), 'w') as f:
            for pl in ('p%03d' % (k - 1), 'p%03d' % k):   # bracketing probes
                if pl not in probes: continue
                g, fg, n = tag_group(nic, pl)
                f.write('ENVPROBE r64=%.2f r256=%.2f group=%s fgfc=%s nn=4 t=0 wall=0\n' % (probes[pl] + (g, 'NA' if fg is None else '%.1f' % fg)))
                print(lab, pl, 'group', g, 'nic files', n)
        g, fg, n = tag_group(nic, lab); print(lab, 'run group', g, 'fgfc %s' % fg)
if __name__ == '__main__': main(sys.argv[1], sys.argv[2])
