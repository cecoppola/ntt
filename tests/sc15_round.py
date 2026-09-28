#!/usr/bin/env python3
"""SC15: the chunk size (MDB_SHIFT_CHUNK_MB / MN_T_CHUNK_MB via estimate --chunk-mb) against the assumed T_ROUND (modelled; run from ecalc/)"""
import sys
sys.path.insert(0, '.')
import mn_model as M, mem_model, estimate as E
fab = M.Fabric(M.TARGET.name, 100.0, 2e-6, write_bw=0.6)
for tr in (0.01, 0.03, 0.1):
    M.T_ROUND = tr
    for c in (1024, 2048, 4096):
        M._PC.clear()
        d = M.Design(np=3, strategy='auto', cap=mem_model.CAPS['2^31'], chunk='both', depth=2, modmul=1, chunk_mb=c, p15=True, round_mb=1024,
                     p15b=True, np_mn='auto', packed=True)
        e = E.estimate(576, 5.1e13 / 576, 'grid', None, fab, 'model', staging='code', design=d)
        print('T_ROUND %.2f s, chunk %4d MB: %.1f / %.1f s, node %.1f GB' % (tr, c, e['nowrite_s'], e['wall_s'], e['node_gb']))
