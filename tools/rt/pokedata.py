# Shared parsers for species / evolution / dex data of the decomp.
import re, os, json
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

def species_ids():
    ids={}
    for l in open(REPO+'/include/constants/species.h'):
        m=re.match(r'#define (SPECIES_\w+)\s+(\d+)\s*$',l)
        if m: ids[m.group(1)]=int(m.group(2))
    return ids

def national_order():
    out=[]
    for l in open(REPO+'/include/constants/pokedex.h'):
        m=re.match(r'\s*NATIONAL_DEX_(\w+),',l)
        if m and m.group(1)!='NONE': out.append('SPECIES_'+m.group(1))
    return out[:386]

def species_info():
    s=open(REPO+'/src/data/pokemon/species_info.h').read()
    info={}
    for m in re.finditer(r'\[(SPECIES_\w+)\]\s*=\s*\n\s*\{(.*?)\n    \}',s,re.S):
        b=m.group(2); d={}
        for k in ['baseHP','baseAttack','baseDefense','baseSpeed','baseSpAttack','baseSpDefense','catchRate']:
            mm=re.search(r'\.'+k+r'\s*=\s*(\d+)',b); d[k]=int(mm.group(1)) if mm else 0
        mm=re.search(r'\.types\s*=\s*\{\s*TYPE_(\w+),\s*TYPE_(\w+)\s*\}',b)
        d['types']=sorted(set([mm.group(1),mm.group(2)]),key=[mm.group(1),mm.group(2)].index) if mm else []
        d['bst']=sum(d[k] for k in ['baseHP','baseAttack','baseDefense','baseSpeed','baseSpAttack','baseSpDefense'])
        mm=re.search(r'\.eggGroups\s*=\s*\{\s*EGG_GROUP_(\w+),\s*EGG_GROUP_(\w+)',b)
        d['egg']=[mm.group(1),mm.group(2)] if mm else []
        info[m.group(1)]=d
    return info

def evolutions():
    s=open(REPO+'/src/data/pokemon/evolution.h').read()
    evo={}
    for m in re.finditer(r'\[(SPECIES_\w+)\]\s*=\s*\{(.*?\})\}',s,re.S):
        for e in re.finditer(r'\{(EVO_\w+),\s*([\w ]+),\s*(SPECIES_\w+)\}',m.group(2)):
            evo.setdefault(m.group(1),[]).append((e.group(1),e.group(2).strip(),e.group(3)))
    return evo
