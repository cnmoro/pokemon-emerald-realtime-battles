import os
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
WORK=os.environ.get('RT_WORK', os.path.join(REPO, 'build', 'rt_work'))
os.makedirs(WORK, exist_ok=True)
import xml.etree.ElementTree as ET, os, sys, hashlib, json, pickle
from PIL import Image
sys.path.insert(0,os.path.dirname(__file__)); import lz
SC=os.path.join(os.environ.get('SPRITECOLLAB', 'SpriteCollab'), 'sprite')
ANIMS=[('Walk',6,5),('Idle',3,5),('Attack',6,5),('Shoot',4,5),('Hurt',2,5),('Sleep',2,1),('Charge',3,5)]
FALLBACK={'Idle':'Walk','Shoot':'Attack','Charge':'Shoot','Sleep':'Hurt'}
SHAPES=[(8,8,0,0),(16,16,0,1),(32,32,0,2),(64,64,0,3),(16,8,1,0),(32,8,1,1),(32,16,1,2),(64,32,1,3),(8,16,2,0),(8,32,2,1),(16,32,2,2),(32,64,2,3)]
def pick_shape(w,h):
    best=None
    for sw,sh,shp,sz in SHAPES:
        if sw>=w and sh>=h:
            if best is None or sw*sh<best[0]*best[1]: best=(sw,sh,shp,sz)
    return best
def load_anims(d):
    r=ET.parse(d+'/AnimData.xml').getroot(); out={}
    for a in r.find('Anims'):
        n=a.find('Name').text; c=a.find('CopyOf')
        if c is not None: out[n]=('copy',c.text); continue
        hf=a.find('HitFrame')
        out[n]=(int(a.find('FrameWidth').text),int(a.find('FrameHeight').text),[int(x.text) for x in a.find('Durations')], int(hf.text) if hf is not None else -1)
    sh=int(r.find('ShadowSize').text)
    return out,sh
def subsample(idx, maxn, keep=-1):
    if len(idx)<=maxn: return list(range(len(idx)))
    sel=sorted(set(round(k*(len(idx)-1)/(maxn-1)) for k in range(maxn)))
    if keep>=0 and keep not in sel:
        # replace nearest
        near=min(range(len(sel)),key=lambda q:abs(sel[q]-keep)); sel[near]=keep; sel=sorted(set(sel))
    return sel
def process(dex, quant_cache=None):
    d='%s/%04d'%(SC,dex); A,shadow=load_anims(d)
    def res(n):
        seen=0
        while n in A and A[n][0]=='copy' and seen<5: n=A[n][1]; seen+=1
        return n if n in A and os.path.exists(d+'/'+n+'-Anim.png') else None
    # palette
    imgs={}
    for an,_,_ in ANIMS:
        n=res(an)
        if n and n not in imgs: imgs[n]=Image.open(d+'/'+n+'-Anim.png').convert('RGBA')
    colors=[]
    for im in imgs.values():
        for px in im.getdata():
            if px[3]>127:
                c=(px[0]>>3,px[1]>>3,px[2]>>3)
                if c not in colors: colors.append(c)
    assert len(colors)<=15, (dex,len(colors))
    # sort by luminance for nicer ordering
    colors.sort(key=lambda c:(c[0]*3+c[1]*6+c[2]))
    cidx={c:i+1 for i,c in enumerate(colors)}
    pal=[(0,0,0)]+colors+[(0,0,0)]*(15-len(colors))
    anims={}; frames=[]
    for an,maxn,rows in ANIMS:
        n=res(an); src=an
        while n is None and src in FALLBACK:
            src=FALLBACK[src]; n=res(src)
        if n is None: n=res('Walk')
        fw,fh,du,hf=A[n]; im=imgs[n]
        nrows=im.size[1]//fh; ncols=len(du)
        sel=subsample(du,maxn,hf)
        # durations: merge skipped frame durations into selected
        seld=[]
        for k,s in enumerate(sel):
            e=sel[k+1] if k+1<len(sel) else len(du)
            seld.append(sum(du[s:e]))
        hitk=0
        if hf>=0:
            hitk=max([k for k,s in enumerate(sel) if s<=hf] or [0])
        dirs=[]
        for r in range(rows):
            rr=min(r,nrows-1)
            fl=[]
            for k,s in enumerate(sel):
                fr=im.crop((s*fw,rr*fh,s*fw+fw,rr*fh+fh))
                fl.append(make_frame(fr,fw,fh,cidx,seld[k]))
            dirs.append(fl)
        anims[an]=dict(dirs=dirs,hit=hitk,n=len(sel))
    return dict(dex=dex,pal=pal,anims=anims,shadow=shadow)
def make_frame(fr,fw,fh,cidx,dur):
    b=fr.getbbox()
    cx=fw//2; cy=fh//2
    if not b: return dict(x=0,y=0,shape=(8,8,0,0),tiles=bytes(32),dur=dur)
    x0,y0,x1,y1=b
    w=x1-x0;h=y1-y0
    if w>64:
        mid=(x0+x1)//2; x0=mid-32; x1=x0+64; w=64
    if h>64:
        # keep bottom (feet) part plus most of body
        y0=y1-64; h=64
    sw,sh,shp,sz=pick_shape(w,h)
    # center the bbox inside shape
    ox=x0-(sw-w)//2; oy=y0-(sh-h)//2
    px=fr.load()
    tiles=bytearray()
    for ty in range(sh//8):
        for tx in range(sw//8):
            for yy in range(8):
                for xx in range(0,8,2):
                    v=[]
                    for q in range(2):
                        X=ox+tx*8+xx+q; Y=oy+ty*8+yy
                        if 0<=X<fw and 0<=Y<fh and X>=x0 and X<x1 and Y>=y0 and Y<y1:
                            p=px[X,Y]
                            v.append(cidx[(p[0]>>3,p[1]>>3,p[2]>>3)] if p[3]>127 else 0)
                        else: v.append(0)
                    tiles.append(v[0]|(v[1]<<4))
    return dict(x=ox-cx,y=oy-cy,shape=(sw,sh,shp,sz),tiles=bytes(tiles),dur=dur)
if __name__=='__main__':
    out={}
    a,b=int(sys.argv[1]),int(sys.argv[2])
    for dex in range(a,b+1):
        out[dex]=process(dex)
    pickle.dump(out,open(sys.argv[3],'wb'))
