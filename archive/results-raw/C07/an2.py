import csv,sys,re,collections
tag=sys.argv[1]
rows=list(csv.DictReader(open(tag+'.csv')))
AG={'Agent 4':0,'Agent 10':1,'Agent 16':2,'Agent 22':3}
iv=collections.defaultdict(list)
for r in rows: iv[AG[r['Agent_Id']]].append((int(r['Start_Timestamp']),int(r['End_Timestamp']),r['Kernel_Name']))
def union(l):
    out=[]
    for s,e in sorted(l):
        if out and s<=out[-1][1]: out[-1][1]=max(out[-1][1],e)
        else: out.append([s,e])
    return out
U={a:union([(s,e) for s,e,_ in v]) for a,v in iv.items()}
def busy(a,w0,w1):
    t=0
    for s,e in U[a]:
        if e<=w0: continue
        if s>=w1: break
        t+=min(e,w1)-max(s,w0)
    return t/1e9
lv=[];pr=[]
for l in open(tag+'.log'):
    m=re.match(r'c07ts level (\d+) (\w+) (\d+) (\d+)',l)
    if m: lv.append((int(m.group(1)),m.group(2),int(m.group(3)),int(m.group(4))))
    m=re.match(r'c07ts prod (\w) (\d+) (\d+) (\d+)',l)
    if m: pr.append((m.group(1),int(m.group(2)),int(m.group(3)),int(m.group(4))))
k0=min(s for a in iv for s,e,n in iv[a]); k1=max(e for a in iv for s,e,n in iv[a])
print('first level start vs kernel range:',(lv[0][2]-k0)/1e9,(k1-lv[0][2])/1e9)
tot=collections.defaultdict(lambda:[0,[0]*4,0])
for n,tier,a,b in lv:
    w=(b-a)/1e9; bz=[busy(i,a,b) for i in range(4)]
    t=tot[tier]; t[0]+=w; t[2]+=1
    for i in range(4): t[1][i]+=bz[i]
    print('lvl %2d %-6s wall %5.2f busy %s idle %s'%(n,tier,w,' '.join('%5.2f'%x for x in bz),' '.join('%5.2f'%(w-x) for x in bz)))
for k,(w,b,c) in tot.items(): print('TOTAL',k,'levels',c,'wall %.2f'%w,'busy',' '.join('%.2f'%x for x in b),'idle',' '.join('%.2f'%(w-x) for x in b))
# products
pt=collections.defaultdict(lambda:[0,0,[0]*4])
for f,nc,a,b in pr:
    w=(b-a)/1e9; t=pt[f]; t[0]+=1; t[1]+=w
    for i in range(4): t[2][i]+=busy(i,a,b)
for f,(c,w,b) in pt.items(): print('PROD',f,'n',c,'wall %.2f'%w,'busy',' '.join('%.2f'%x for x in b),'idle',' '.join('%.2f'%(w-x) for x in b))
