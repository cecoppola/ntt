import csv,sys,re,collections
tag=sys.argv[1]
rows=list(csv.DictReader(open(tag+'.csv')))
AG={'Agent 4':0,'Agent 5':1,'Agent 6':2,'Agent 7':3}
comp=collections.defaultdict(list);push=collections.defaultdict(list); names=collections.defaultdict(list)
for r in rows:
    a=AG[r['Agent_Id']]; s,e=int(r['Start_Timestamp']),int(r['End_Timestamp']); n=r['Kernel_Name']
    (push if 'k_push' in n else comp)[a].append((s,e)); names[a].append((s,e,n))
def union(l):
    out=[]
    for s,e in sorted(l):
        if out and s<=out[-1][1]: out[-1][1]=max(out[-1][1],e)
        else: out.append([s,e])
    return out
C={a:union(v) for a,v in comp.items()}; P={a:union(v) for a,v in push.items()}
def clip(U,w0,w1): return [[max(s,w0),min(e,w1)] for s,e in U if e>w0 and s<w1]
def length(U): return sum(e-s for s,e in U)/1e9
def inter(A,B):
    i=j=0;t=0
    while i<len(A) and j<len(B):
        s=max(A[i][0],B[j][0]); e=min(A[i][1],B[j][1])
        if e>s: t+=e-s
        if A[i][1]<B[j][1]: i+=1
        else: j+=1
    return t/1e9
prods=[]
for l in open(tag+'.log'):
    m=re.match(r'c07ts prod (\w) (\d+) (\d+) (\d+)',l)
    if m: prods.append((m.group(1),int(m.group(2)),int(m.group(3)),int(m.group(4))))
W=sum((b-a) for _,_,a,b in prods)/1e9
print('products',len(prods),'wall %.2f'%W)
for a in range(4):
    c=p=ov=0
    for _,_,w0,w1 in prods:
        cc=clip(C[a],w0,w1); pp=clip(P.get(a,[]),w0,w1)
        c+=length(cc); p+=length(pp); ov+=inter(cc,pp)
    union_=c+p-ov
    print('APU%d: compute %.2f push %.2f overlap(push under compute) %.2f => push-exposed(only) %.2f, idle(neither) %.2f of %.2f'%(a,c,p,ov,p-ov,W-union_,W))
# gap attribution on APU0: per product, gaps >50us between compute kernels, by (prev kernel, next kernel)
g=collections.Counter(); gn=collections.Counter()
for _,_,w0,w1 in prods:
    ks=sorted([x for x in names[0] if x[0]>=w0 and x[1]<=w1 and 'k_push' not in x[2]])
    pu=P.get(0,[])
    for (s0,e0,n0),(s1,e1,n1) in zip(ks,ks[1:]):
        if s1>e0:
            gap=(s1-e0)
            # subtract overlap with push
            ov=inter([[e0,s1]],clip(pu,e0,s1))*1e9
            key=(re.sub(r'\(.*','',n0)[:26],re.sub(r'\(.*','',n1)[:26])
            g[key]+=(gap-ov)/1e9; gn[key]+=ov/1e9
print('APU0 compute-gap with nothing running, by (prev -> next):')
for k,v in g.most_common(8): print('  %-60s idle %.3f   (push-covered %.3f)'%(' -> '.join(k),v,gn[k]))
print('APU0 gap covered by push only, by (prev->next):')
for k,v in gn.most_common(8): print('  %-60s %.3f'%(' -> '.join(k),v))
print('--- per product size (APU0 view; mean over APUs of idle)')
by=collections.defaultdict(lambda:[0,0.0,0.0,0.0,0.0])
for f,nc,w0,w1 in prods:
    w=(w1-w0)/1e9; idl=0;cmp_=0;pe=0
    for a in range(4):
        cc=clip(C[a],w0,w1); pp=clip(P.get(a,[]),w0,w1)
        u=length(cc)+length(pp)-inter(cc,pp); idl+=(w-u)/4; cmp_+=length(cc)/4; pe+=(length(pp)-inter(cc,pp))/4
    k=nc; b=by[k]; b[0]+=1;b[1]+=w;b[2]+=idl;b[3]+=cmp_;b[4]+=pe
for k in sorted(by):
    n,w,i,c,pe=by[k]
    print('nc %11d  n %2d  wall/prod %7.1f ms  idle %5.1f%%  compute %5.1f%%  push-only %4.1f%%  idle ms/prod %.1f'%(k,n,1e3*w/n,100*i/w,100*c/w,100*pe/w,1e3*i/n))
print('--- aggregate nc>=1e8 and 1e7..1e8')
for lo,hi in [(1e8,1e11),(1e7,1e8),(0,1e7)]:
    W=I=Cc=Pe=Pt=0;n=0
    for f,nc,w0,w1 in prods:
        if not lo<=nc<hi: continue
        n+=1; w=(w1-w0)/1e9; W+=w
        for a in range(4):
            cc=clip(C[a],w0,w1); pp=clip(P.get(a,[]),w0,w1)
            u=length(cc)+length(pp)-inter(cc,pp); I+=(w-u)/4; Cc+=length(cc)/4; Pe+=(length(pp)-inter(cc,pp))/4; Pt+=length(pp)/4
    print('nc %g-%g n=%d wall %.2f compute %.1f%% push-total %.1f%% push-exposed %.1f%% (=%.0f%% of push) idle %.1f%%'%(lo,hi,n,W,100*Cc/W,100*Pt/W,100*Pe/W,100*Pe/Pt,100*I/W))
