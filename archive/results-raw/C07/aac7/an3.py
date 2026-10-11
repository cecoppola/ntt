import csv,sys,collections,re
rows=list(csv.DictReader(open(sys.argv[1]+'.csv')))
AG={'Agent 4':0,'Agent 5':1,'Agent 6':2,'Agent 7':3}
d=collections.defaultdict(lambda:[0]*4)
for r in rows: d[re.sub(r'\(.*','',r['Kernel_Name'])[:46]][AG[r['Agent_Id']]]+=(int(r['End_Timestamp'])-int(r['Start_Timestamp']))/1e9
tot=[sum(v[i] for v in d.values()) for i in range(4)]
print('TOTAL busy sum of kernels',' '.join('%.2f'%x for x in tot))
for k,v in sorted(d.items(),key=lambda kv:-abs(kv[1][0]-kv[1][3]))[:int(sys.argv[2])]:
    print('%-48s'%k,' '.join('%6.3f'%x for x in v),' diff0-3 %.3f'%(v[0]-v[3]))
