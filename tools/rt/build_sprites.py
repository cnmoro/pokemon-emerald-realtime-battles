import os
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
WORK=os.environ.get('RT_WORK', os.path.join(REPO, 'build', 'rt_work'))
os.makedirs(WORK, exist_ok=True)
import pickle, struct, subprocess, colorsys, os, re
PE=REPO
# RT_LC=1: Liquid Crystal build, 411 sprites (its species 252-276 are Turtwig..Bastiodon, national 387-411)
LC=os.environ.get('RT_LC')=='1'
DEXMAX=411 if LC else 386
SUFFIX='_lc' if LC else ''
d={};[d.update(pickle.load(open(os.path.join(WORK, '%s.pkl'%x),'rb'))) for x in ('abc' if LC else 'ab')]
ANIM_ORDER=['Walk','Idle','Attack','Shoot','Hurt','Sleep','Charge']
# dex -> folder name
names=[]
for line in open(PE+'/include/constants/pokedex.h'):
    m=re.match(r'\s*NATIONAL_DEX_(\w+),',line)
    if m and m.group(1)!='NONE': names.append(m.group(1).lower())
names=names[:386]
def readpal(p):
    L=open(p).read().split('\n'); n=int(L[2]); return [tuple(int(v) for v in L[3+i].split()) for i in range(n)]
def shiny_pal(dex,pal):
    if dex>386:
        return lc_shiny_pal(dex,pal)
    nm=names[dex-1]; f=PE+'/graphics/pokemon/'+nm
    if nm=='castform': f+='/normal'
    if not os.path.exists(f+'/normal.pal'):
        print('no pal',nm); return pal
    N=readpal(f+'/normal.pal'); S=readpal(f+'/shiny.pal')
    out=[pal[0]]
    for c in pal[1:]:
        r,g,b=[v*255/31 for v in c]
        best=min(range(1,len(N)),key=lambda i:(N[i][0]-r)**2*3+(N[i][1]-g)**2*4+(N[i][2]-b)**2*2)
        n=N[best]; s=S[best]
        if n==s: out.append(c); continue
        ch,cs,cv=colorsys.rgb_to_hsv(r/255,g/255,b/255)
        nh,ns,nv=colorsys.rgb_to_hsv(*[x/255 for x in n]); sh,ss,sv=colorsys.rgb_to_hsv(*[x/255 for x in s])
        h=(ch+(sh-nh))%1.0 if ns>0.08 and cs>0.08 else sh
        sat=min(1,cs*(ss/ns)) if ns>0.05 else ss if cs<0.05 else min(1,cs+(ss-ns))
        val=min(1,cv*(sv/nv)) if nv>0.05 else sv
        R,G,B=colorsys.hsv_to_rgb(h,max(0,sat),max(0,val))
        out.append((min(31,round(R*31)),min(31,round(G*31)),min(31,round(B*31))))
    return out
def lz77(data,o):
    n=data[o+1]|(data[o+2]<<8)|(data[o+3]<<16); o+=4; out=bytearray()
    while len(out)<n:
        flags=data[o]; o+=1
        for b in range(8):
            if len(out)>=n: break
            if flags&(0x80>>b):
                x=data[o]<<8|data[o+1]; o+=2
                ln=(x>>12)+3; disp=(x&0xFFF)+1
                for _ in range(ln): out.append(out[-disp])
            else:
                out.append(data[o]); o+=1
    return bytes(out)
def lc_pal(table,species):
    rom=LCROM; e=table+species*8
    ptr=struct.unpack_from('<I',rom,e)[0]&0x1FFFFFF
    raw=lz77(rom,ptr)
    return [((v&31),(v>>5)&31,(v>>10)&31) for v in struct.unpack_from('<16H',raw)]
def lc_shiny_pal(dex,pal):
    # map each sprite colour to the closest colour of Liquid Crystal's own normal palette, take the shiny one
    species=dex-387+252
    N=lc_pal(0x0823730c&0x1FFFFFF,species); S=lc_pal(0x082380cc&0x1FFFFFF,species)
    N=[tuple(v*255//31 for v in c) for c in N]; S=[tuple(v*255//31 for v in c) for c in S]
    out=[pal[0]]
    for c in pal[1:]:
        r,g,b=[v*255/31 for v in c]
        best=min(range(1,16),key=lambda i:(N[i][0]-r)**2*3+(N[i][1]-g)**2*4+(N[i][2]-b)**2*2)
        n=N[best]; sc=S[best]
        if n==sc: out.append(c); continue
        ch,cs,cv=colorsys.rgb_to_hsv(r/255,g/255,b/255)
        nh,ns,nv=colorsys.rgb_to_hsv(*[x/255 for x in n]); sh,ss,sv=colorsys.rgb_to_hsv(*[x/255 for x in sc])
        h=(ch+(sh-nh))%1.0 if ns>0.08 and cs>0.08 else sh
        sat=min(1,cs*(ss/ns)) if ns>0.05 else ss if cs<0.05 else min(1,cs+(ss-ns))
        val=min(1,cv*(sv/nv)) if nv>0.05 else sv
        R,G,B=colorsys.hsv_to_rgb(h,max(0,sat),max(0,val))
        out.append((min(31,round(R*31)),min(31,round(G*31)),min(31,round(B*31))))
    return out
LCROM=open(os.environ['LC_ROM'],'rb').read() if LC else None
def rgb555(c): return c[0]|(c[1]<<5)|(c[2]<<10)
# unique blobs
blobs={}; order=[]
for dex in range(1,DEXMAX+1):
    for an in ANIM_ORDER:
        for fl in d[dex]['anims'][an]['dirs']:
            for f in fl:
                if f['tiles'] not in blobs: blobs[f['tiles']]=len(order); order.append(f['tiles'])
print('unique',len(order))
inp=b''.join(struct.pack('<I',len(t))+t for t in order)
res=subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lzc')],input=inp,capture_output=True).stdout
offs=[];bin_=bytearray();p=0
for i in range(len(order)):
    n=struct.unpack_from('<I',res,p)[0]; p+=4
    offs.append(len(bin_)); bin_+=res[p:p+n]; p+=n
os.makedirs(PE+'/graphics/rt',exist_ok=True); os.makedirs(PE+'/src/data/rt',exist_ok=True)
open(PE+'/graphics/rt/sprites%s.bin'%SUFFIX,'wb').write(bin_)
print('bin bytes',len(bin_))
# tables
fr=[];sp=[]
for dex in range(1,DEXMAX+1):
    s=d[dex]; starts=[];counts=[];hits=[]
    for an in ANIM_ORDER:
        a=s['anims'][an]; starts.append(len(fr)); counts.append(a['n']); hits.append(a['hit'])
        dirs=a['dirs']
        for di in range(5):
            for f in dirs[min(di,len(dirs)-1)]:
                sw,sh,shp,sz=f['shape']
                fr.append('{%d,%d,%d,%d,%d}'%(offs[blobs[f['tiles']]],max(-128,min(127,f['x'])),max(-128,min(127,f['y'])),(shp<<2)|sz|(f.get('aff',0)<<4),min(255,f['dur'])))
    # metrics from walk down frame 0: real pixel bbox
    def bbox(f):
        if f.get('dbox'): return f['dbox']
        sw,sh=f['shape'][:2]; t=f['tiles']; tw=sw//8
        xs=[];ys=[]
        for ty in range(sh//8):
            for tx in range(tw):
                base=(ty*tw+tx)*32
                for yy in range(8):
                    for xx in range(8):
                        b=t[base+yy*4+xx//2]; v=(b>>4) if xx&1 else (b&15)
                        if v: xs.append(tx*8+xx); ys.append(ty*8+yy)
        if not xs: return (0,0,8,8)
        return (min(xs)+f['x'],min(ys)+f['y'],max(xs)+1+f['x'],max(ys)+1+f['y'])
    b0=bbox(s['anims']['Walk']['dirs'][0][0]); b2=bbox(s['anims']['Walk']['dirs'][2][0])
    wdt=((b0[2]-b0[0])+(b2[2]-b2[0]))/2
    radius=int(max(6,min(26,wdt*0.42)))
    height=int(max(8,min(60,-b0[1])))
    pal=s['pal']; shp=shiny_pal(dex,pal)
    sp.append(dict(starts=starts,counts=counts,hits=hits,pal=pal,shiny=shp,shadow=s['shadow'],radius=int(radius),height=int(height)))
with open(PE+'/src/data/rt/species_gfx%s.h'%SUFFIX,'w') as o:
    o.write('// generated by build_sprites.py from PMD SpriteCollab (https://sprites.pmdcollab.org) - CC BY-NC 4.0 credits in docs\n')
    o.write('const struct RtFrame gRtFrames[%d] = {\n'%len(fr))
    for i in range(0,len(fr),8): o.write(','.join(fr[i:i+8])+',\n')
    o.write('};\n')
    o.write('const struct RtSpeciesGfx gRtSpeciesGfx[RT_GFX_DEX_COUNT + 1] = {\n  {0},\n')
    for s in sp:
        o.write('  {{%s},{%s},{%s},%d,%d,%d},\n'%(','.join(map(str,s['starts'])),','.join(map(str,s['counts'])),','.join(map(str,s['hits'])),s['shadow'],s['radius'],s['height']))
    o.write('};\n')
    o.write('const u16 gRtSpeciesPal[RT_GFX_DEX_COUNT + 1][2][16] = {\n  {{0},{0}},\n')
    for s in sp:
        o.write('  {{%s},{%s}},\n'%(','.join('0x%04X'%rgb555(c) for c in s['pal']),','.join('0x%04X'%rgb555(c) for c in s['shiny'])))
    o.write('};\n')
print('frames',len(fr))
# preview sheet of shiny vs normal for a few
from PIL import Image
