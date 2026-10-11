import os, sys
sys.path.insert(0, '.')
import mem_model as m
nq = 2061111111557
cases = {'default 2,4,..,64,192,576': [2,4,8,16,32,64,192,576], 'one general level 2..64,576': [2,4,8,16,32,64,576],
 'cut: 2..64,512,576 (576=512+64)': [2,4,8,16,32,64,512,576], 'cut: 2..64,256,576 (2x256+64)': [2,4,8,16,32,64,256,576],
 'cut: 2..64,384,576 (384+192)': [2,4,8,16,32,64,384,576], 'cut + 3 levels: 2..32,96,384,576': [2,4,8,16,32,96,384,576],
 'no cut, 3 levels: 2..32,96,192,576': [2,4,8,16,32,96,192,576]}
for name, gr in cases.items():
    row = []
    for sh in ('0', '1'):
        os.environ['COMM_LAYER_VSLOT_SHARE'] = sh
        r = m.vslot_resident(nq, 576, groups=gr)
        row.append(r)
    a, b = row
    print(f"{name}: levels {[(l,g,round(v/1e9,3)) for l,g,v in a['levels']]} dm {a['dm']/1e9:.3f} | SHARE=0 per_apu {a['per_apu']/1e9:.3f} GB  SHARE=1 per_apu {b['per_apu']/1e9:.3f} GB  saving/node {4*(a['per_apu']-b['per_apu'])/1e9:.2f} GB")
