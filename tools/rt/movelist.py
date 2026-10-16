import os
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
WORK=os.environ.get('RT_WORK', os.path.join(REPO, 'build', 'rt_work'))
os.makedirs(WORK, exist_ok=True)
import re, json
PE=REPO
src=open(PE+'/src/data/battle_moves.h').read()
moves={}
for m in re.finditer(r'\[(MOVE_\w+)\]\s*=\s*\{(.*?)\n    \}', src, re.S):
    name=m.group(1); body=m.group(2); d={}
    for k in ['effect','power','type','accuracy','pp','secondaryEffectChance','target','priority','flags']:
        mm=re.search(r'\.'+k+r'\s*=\s*([^,\n]+(?:\|[^,\n]+)*),', body)
        d[k]=mm.group(1).strip() if mm else '0'
    moves[name]=d
ids={}
for line in open(PE+'/include/constants/moves.h'):
    mm=re.match(r'#define (MOVE_\w+)\s+(\d+)',line)
    if mm: ids[mm.group(1)]=int(mm.group(2))
out=[]
for n,i in sorted(ids.items(),key=lambda x:x[1]):
    if n in moves: out.append(dict(name=n,id=i,**moves[n]))
json.dump(out,open(os.path.join(WORK, 'moves.json'),'w'),indent=0)
print(len(out))
for o in out: print(o['id'],o['name'][5:],o['effect'][7:],o['power'],o['type'][5:],o['accuracy'],o['pp'],o['target'][12:],'C' if 'CONTACT' in o['flags'] else '')
