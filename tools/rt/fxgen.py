import os
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
WORK=os.environ.get('RT_WORK', os.path.join(REPO, 'build', 'rt_work'))
os.makedirs(WORK, exist_ok=True)
#!/usr/bin/env python3
"""Procedural pixel-art FX atlas for the real-time battle engine.
Shapes are drawn with ramp indices 1..15 (dark->bright) so a palette recolors them.
UI shapes use fixed indices of the UI palette.
Outputs: graphics/rt/fx_atlas.bin (raw 4bpp tiles, 1D OBJ order), src/data/rt/fx_ids.h, src/data/rt/fx_data.h
"""
import math, random, os, sys
from PIL import Image

PE = REPO
random.seed(1234)

SHAPES_SZ = {(8,8):(0,0),(16,16):(0,1),(32,32):(0,2),(64,64):(0,3),(16,8):(1,0),(32,8):(1,1),(32,16):(1,2),(64,32):(1,3),(8,16):(2,0),(8,32):(2,1),(16,32):(2,2),(32,64):(2,3)}

class Canvas:
    def __init__(s, w, h):
        s.w, s.h = w, h
        s.p = [[0]*w for _ in range(h)]
    def put(s, x, y, v):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < s.w and 0 <= y < s.h:
            s.p[y][x] = max(0, min(15, int(v)))
    def get(s, x, y):
        if 0 <= x < s.w and 0 <= y < s.h: return s.p[y][x]
        return 0
    def putmax(s, x, y, v):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < s.w and 0 <= y < s.h and v > s.p[y][x]:
            s.p[y][x] = max(0, min(15, int(v)))
    def outline(s, v=1):
        q = [row[:] for row in s.p]
        for y in range(s.h):
            for x in range(s.w):
                if s.p[y][x] == 0:
                    for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
                        if s.get(x+dx, y+dy) > v:
                            q[y][x] = v; break
        s.p = q
    def tiles(s):
        out = bytearray()
        for ty in range(s.h//8):
            for tx in range(s.w//8):
                for y in range(8):
                    for x in range(0, 8, 2):
                        a = s.p[ty*8+y][tx*8+x]; b = s.p[ty*8+y][tx*8+x+1]
                        out.append(a | (b << 4))
        return bytes(out)

def ramp(t):  # t 0..1 -> 2..15
    return 2 + int(round(max(0, min(1, t)) * 13))

def disc(c, cx, cy, r, core=1.0, hl=True, falloff=1.0):
    for y in range(c.h):
        for x in range(c.w):
            d = math.hypot(x+0.5-cx, y+0.5-cy)
            if d <= r:
                t = 1 - (d / r) ** falloff
                if hl:
                    # highlight top-left
                    hd = math.hypot(x+0.5-(cx-r*0.35), y+0.5-(cy-r*0.35))
                    t = max(t, 1 - hd/(r*0.6)) if hd < r*0.6 else t
                c.putmax(x, y, ramp(t*core))

def ring(c, cx, cy, r, th=1.5, v=12):
    for y in range(c.h):
        for x in range(c.w):
            d = math.hypot(x+0.5-cx, y+0.5-cy)
            if abs(d - r) <= th:
                t = 1 - abs(d-r)/th
                c.putmax(x, y, ramp(0.3 + 0.7*t) if v is None else max(2, int(v*(0.5+0.5*t))))

def line(c, x0, y0, x1, y1, v, w=1.0):
    n = int(max(abs(x1-x0), abs(y1-y0))*2)+1
    for i in range(n+1):
        t = i/n; x = x0+(x1-x0)*t; y = y0+(y1-y0)*t
        if w <= 1.0: c.putmax(x, y, v)
        else:
            rr = w/2
            for yy in range(int(y-rr)-1, int(y+rr)+2):
                for xx in range(int(x-rr)-1, int(x+rr)+2):
                    if math.hypot(xx+0.5-x-0.5, yy+0.5-y-0.5) <= rr: c.putmax(xx, yy, v)

def poly(c, pts, vfun):
    xs=[p[0] for p in pts]; ys=[p[1] for p in pts]
    for y in range(int(min(ys)), int(max(ys))+1):
        for x in range(int(min(xs)), int(max(xs))+1):
            px, py = x+0.5, y+0.5; inside=False; j=len(pts)-1
            for i in range(len(pts)):
                xi,yi=pts[i]; xj,yj=pts[j]
                if ((yi>py)!=(yj>py)) and (px < (xj-xi)*(py-yi)/(yj-yi+1e-9)+xi): inside = not inside
                j=i
            if inside: c.putmax(x, y, vfun(px, py))

SHAPES = []  # (name, w, h, [canvases], ui)

def add(name, w, h, frames, ui=False):
    SHAPES.append((name, w, h, frames, ui))

# ---------------------------------------------------------------- shapes
def mk_orb(sz, frames=2):
    fr=[]
    for f in range(frames):
        c=Canvas(sz,sz); r=sz/2-1-(f*0.5 if sz>8 else 0)
        disc(c, sz/2, sz/2, r, core=1.0)
        if sz>=16:
            for k in range(3+sz//8):
                a=random.random()*6.28; d=r*random.uniform(0.3,0.8)
                c.putmax(sz/2+math.cos(a)*d, sz/2+math.sin(a)*d, 15)
        c.outline(1); fr.append(c)
    return fr
add('ORB8',8,8,mk_orb(8))
add('ORB16',16,16,mk_orb(16))
add('ORB32',32,32,mk_orb(32,1))

def mk_spark():
    fr=[]
    for f,s in enumerate([1.5,3.5,2.5,1.0]):
        c=Canvas(8,8)
        for k in range(-4,5):
            t=1-abs(k)/4.5
            if abs(k)<=s: c.putmax(4+k,4,ramp(t)); c.putmax(4,4+k,ramp(t))
        c.putmax(4,4,15); fr.append(c)
    return fr
add('SPARK8',8,8,mk_spark())

def mk_flame(sz, frames):
    fr=[]
    for f in range(frames):
        c=Canvas(sz,sz); cx=sz/2; base=sz*0.85
        for y in range(sz):
            for x in range(sz):
                ny=(base-y)/(sz*0.8)
                if ny<0 or ny>1: continue
                wob=math.sin(ny*7+f*1.7)*sz*0.06
                w=(sz*0.42)*(1-ny)**0.6*(1+0.3*math.sin(f+ny*3))
                dx=abs(x+0.5-cx-wob)
                if dx<w:
                    t=(1-dx/w)*0.7+(1-ny)*0.4+random.uniform(-0.08,0.08)
                    c.putmax(x,y,ramp(t))
        c.outline(1); fr.append(c)
    return fr
add('FLAME16',16,16,mk_flame(16,4))
add('FLAME32',32,32,mk_flame(32,2))

def mk_bolt(w,h,frames,vert=False):
    fr=[]
    for f in range(frames):
        c=Canvas(w,h)
        if vert:
            x=w/2; pts=[(x,0)]
            for y in range(4,h+1,4):
                x=max(3,min(w-4,x+random.choice([-3,-2,2,3]))); pts.append((x,y))
        else:
            y=h/2; pts=[(0,y)]
            for x in range(3,w+1,3):
                y=max(3,min(h-4,y+random.choice([-3,-2,2,3]))); pts.append((x,y))
        for i in range(len(pts)-1):
            line(c,*pts[i],*pts[i+1],8,w=3.0)
        for i in range(len(pts)-1):
            line(c,*pts[i],*pts[i+1],15,w=1.0)
        c.outline(1); fr.append(c)
    return fr
add('BOLT16',16,16,mk_bolt(16,16,3))
add('BOLTV',16,32,mk_bolt(16,32,3,vert=True))

def mk_leaf(sz=16,frames=4):
    fr=[]
    for f in range(frames):
        c=Canvas(sz,sz); a=f*math.pi/4
        for y in range(sz):
            for x in range(sz):
                dx=x+0.5-sz/2; dy=y+0.5-sz/2
                u=dx*math.cos(a)+dy*math.sin(a); v=-dx*math.sin(a)+dy*math.cos(a)
                L=sz*0.45; W=sz*0.2*(1-(u/L)**2) if abs(u)<L else -1
                if W>0 and abs(v)<W:
                    t=0.4+0.5*(1-abs(v)/W)
                    if abs(v)<0.6: t=0.95
                    c.putmax(x,y,ramp(t))
        c.outline(1); fr.append(c)
    return fr
add('LEAF16',16,16,mk_leaf())

def mk_rock(sz,frames):
    fr=[]
    for f in range(frames):
        c=Canvas(sz,sz); n=8; pts=[]
        for k in range(n):
            a=k*2*math.pi/n+random.uniform(-0.2,0.2); r=sz*random.uniform(0.32,0.46)
            pts.append((sz/2+math.cos(a)*r, sz/2+math.sin(a)*r*0.85))
        poly(c,pts,lambda px,py:ramp(0.75-0.5*((px+py)/(2*sz))+random.uniform(-0.07,0.07)))
        c.outline(1); fr.append(c)
    return fr
add('ROCK16',16,16,mk_rock(16,2))
add('ROCK32',32,32,mk_rock(32,1))

def mk_drop():
    c=Canvas(8,8)
    for y in range(8):
        for x in range(8):
            dx=x+0.5-4; dy=y+0.5-5
            r=2.6
            if dy<0: r=2.6*(1+dy/4.5)
            if math.hypot(dx,dy if dy>0 else 0)<r and dy>-4.5: c.putmax(x,y,ramp(0.5+0.4*(-dx-dy)/5))
    c.putmax(3,4,15); c.outline(1); return [c]
add('DROP8',8,8,mk_drop())

def mk_bubble():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); r=6.5-f*0.5
        ring(c,8,8,r,1.2,None)
        for y in range(16):
            for x in range(16):
                if math.hypot(x+0.5-8,y+0.5-8)<r-1: c.putmax(x,y,3)
        c.putmax(5,5,15); c.putmax(6,5,14); c.putmax(5,6,14)
        fr.append(c)
    return fr
add('BUBBLE16',16,16,mk_bubble())

def mk_shard():
    fr=[]
    for f in range(2):
        c=Canvas(16,16)
        if f==0: pts=[(8,1),(12,8),(8,15),(4,8)]
        else: pts=[(2,8),(8,4),(14,8),(8,12)]
        poly(c,pts,lambda px,py:ramp(0.95-0.6*abs(px-8)/7-0.2*py/16))
        c.outline(1); fr.append(c)
    return fr
add('SHARD16',16,16,mk_shard())

def mk_flake():
    fr=[]
    for f in range(2):
        c=Canvas(8,8)
        for k in range(3):
            a=k*math.pi/3+f*math.pi/6
            line(c,4-math.cos(a)*3.5,4-math.sin(a)*3.5,4+math.cos(a)*3.5,4+math.sin(a)*3.5,13)
        c.putmax(4,4,15); fr.append(c)
    return fr
add('FLAKE8',8,8,mk_flake())

def mk_star(sz=16,frames=2):
    fr=[]
    for f in range(frames):
        c=Canvas(sz,sz); pts=[]
        for k in range(10):
            a=-math.pi/2+k*math.pi/5+f*0.3; r=(sz*0.46 if k%2==0 else sz*0.2)
            pts.append((sz/2+math.cos(a)*r,sz/2+math.sin(a)*r))
        poly(c,pts,lambda px,py:ramp(1-math.hypot(px-sz/2,py-sz/2)/(sz*0.5)+0.25))
        c.outline(1); fr.append(c)
    return fr
add('STAR16',16,16,mk_star())

def mk_note():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); o=f
        disc(c,5,11+o*0,3.2,hl=False)
        line(c,8,11,8,2,12,w=1.5)
        line(c,8,2,12,5,12,w=1.5)
        if f==1: line(c,8,5,12,8,12,w=1.5)
        c.outline(1); fr.append(c)
    return fr
add('NOTE16',16,16,mk_note())

def mk_heart():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); s=1.0 if f==0 else 0.85
        for y in range(16):
            for x in range(16):
                X=(x+0.5-8)/(6.5*s); Y=-(y+0.5-8.5)/(6.5*s)
                if (X*X+Y*Y-1)**3 - X*X*Y**3 <= 0: c.putmax(x,y,ramp(0.9-0.4*math.hypot(X+0.3,Y-0.3)))
        c.outline(1); fr.append(c)
    return fr
add('HEART16',16,16,mk_heart())

def mk_ring(sz):
    c=Canvas(sz,sz); ring(c,sz/2,sz/2,sz/2-2,1.6 if sz>16 else 1.2,None); return [c]
add('RING16',16,16,mk_ring(16))
add('RING32',32,32,mk_ring(32))

def mk_slash():
    fr=[]
    for f in range(2):
        c=Canvas(32,32)
        for k in range(3):
            off=(k-1)*6
            a0=-1.0+f*0.25; a1=a0+1.6
            for i in range(30):
                t=i/29; a=a0+(a1-a0)*t
                r=12
                x=10+math.cos(a)*r; y=16+off*0.0+math.sin(a)*r+off
                w=1.0+2.0*math.sin(t*math.pi)
                for d in range(-1,2):
                    c.putmax(x+d*0.3,y,ramp(0.5+0.5*math.sin(t*math.pi)))
                if w>1.6: c.putmax(x+1,y,ramp(0.9*math.sin(t*math.pi)))
        c.outline(1); fr.append(c)
    return fr
add('SLASH32',32,32,mk_slash())

def mk_fist():
    c=Canvas(16,16)
    for y in range(3,13):
        for x in range(2,14):
            if (x in(2,13) and y in(3,12)): continue
            c.putmax(x,y,ramp(0.8-0.3*(y-3)/10))
    for k in range(4): line(c,5+k*2.5,3,5+k*2.5,7,4)
    line(c,3,9,8,9,4)
    c.outline(1); return [c]
add('FIST16',16,16,mk_fist())

def mk_impact():
    fr=[]
    for f in range(2):
        c=Canvas(32,32); n=8; pts=[]
        R=[11,15][f]; r=[5,8][f]
        for k in range(2*n):
            a=k*math.pi/n+(0.2 if f==1 else 0); rr=R if k%2==0 else r
            pts.append((16+math.cos(a)*rr,16+math.sin(a)*rr))
        if f<2: poly(c,pts,lambda px,py:ramp(1.1-math.hypot(px-16,py-16)/R))
        else:
            poly(c,pts,lambda px,py:ramp(0.8-math.hypot(px-16,py-16)/R) if math.hypot(px-16,py-16)>5 else 0)
        c.outline(1); fr.append(c)
    return fr
add('IMPACT32',32,32,mk_impact())

def mk_smoke():
    fr=[]
    for f in range(3):
        c=Canvas(16,16); r=3+f*1.6
        for k in range(4):
            a=k*1.57+f*0.4; d=f*1.3
            disc(c,8+math.cos(a)*d,8+math.sin(a)*d,r,core=0.75,hl=True)
        c.outline(1); fr.append(c)
    return fr
add('SMOKE16',16,16,mk_smoke())

def mk_powder():
    fr=[]
    for f in range(2):
        c=Canvas(8,8)
        for k in range(4):
            c.putmax(random.randint(1,6),random.randint(1,6),ramp(random.uniform(0.6,1)))
        fr.append(c)
    return fr
add('POWDER8',8,8,mk_powder())

def mk_beam():
    fr=[]
    for f in range(2):
        c=Canvas(16,16)
        for y in range(16):
            for x in range(16):
                d=abs(y+0.5-8)
                w=5.0+0.8*math.sin((x+f*4)*0.8)
                if d<w: c.putmax(x,y,ramp(1-d/w+0.15))
        fr.append(c)
    return fr
add('BEAM16',16,16,mk_beam())

def mk_wave():
    fr=[]
    for f in range(2):
        c=Canvas(32,16)
        for x in range(32):
            top=6+3*math.sin((x+f*5)*0.4)
            for y in range(16):
                if y>=top:
                    t=1-(y-top)/(16-top)
                    if y-top<2: t=1.0
                    c.putmax(x,y,ramp(0.35+0.65*t))
        c.outline(1); fr.append(c)
    return fr
add('WAVE32',32,16,mk_wave())

def mk_tornado():
    fr=[]
    for f in range(2):
        c=Canvas(32,32)
        for y in range(2,31):
            w=3+(30-y)*0.42
            for k in range(3):
                ph=y*0.45+f*2.1+k*2.1
                x=16+math.sin(ph)*w
                c.putmax(x,y,ramp(0.5+0.5*math.cos(ph)))
                c.putmax(x+1,y,ramp(0.3+0.5*math.cos(ph)))
        c.outline(1); fr.append(c)
    return fr
add('TORNADO32',32,32,mk_tornado()[:1])  # 1 frame: animated by h-flip

def mk_wisp():
    fr=[]
    for f in range(3):
        c=Canvas(16,16)
        for y in range(16):
            for x in range(16):
                dy=y+0.5-10; dx=x+0.5-8-math.sin(y*0.6+f*2)*1.5
                r=4.5 if dy>0 else 4.5*(1+dy/9)
                if math.hypot(dx,max(dy,0)*1.2)<r and dy>-9: c.putmax(x,y,ramp(0.9-math.hypot(dx,dy)/8))
        c.putmax(6,10,1); c.putmax(9,10,1)
        c.outline(1); fr.append(c)
    return fr
add('WISP16',16,16,mk_wisp())

def mk_psyring():
    fr=[]
    for f in range(2):
        c=Canvas(16,16)
        ring(c,8,8,6.5-f,1.0,None); ring(c,8,8,3.5-f*0.5,0.9,None)
        fr.append(c)
    return fr
add('PSYRING16',16,16,mk_psyring())

def mk_bone():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); a=f*math.pi/4
        x0=8-math.cos(a)*5; y0=8-math.sin(a)*5; x1=8+math.cos(a)*5; y1=8+math.sin(a)*5
        line(c,x0,y0,x1,y1,12,w=2.0)
        for (px,py) in ((x0,y0),(x1,y1)):
            disc(c,px-math.sin(a)*1.5,py+math.cos(a)*1.5,1.8,hl=False); disc(c,px+math.sin(a)*1.5,py-math.cos(a)*1.5,1.8,hl=False)
        c.outline(1); fr.append(c)
    return fr
add('BONE16',16,16,mk_bone())

def mk_needle():
    c=Canvas(8,8); line(c,0,4,7,4,10,w=1.0); c.putmax(7,4,15); c.putmax(6,4,14); c.putmax(0,3,6); c.putmax(0,5,6); c.outline(1); return [c]
add('NEEDLE8',8,8,mk_needle())

def mk_feather():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); a=-0.6+f*0.6
        for i in range(24):
            t=i/23; x=8+math.cos(a)*(t*12-6); y=8+math.sin(a)*(t*12-6)
            w=3*math.sin(t*math.pi)
            for d in range(-3,4):
                if abs(d)<=w: c.putmax(x-math.sin(a)*d,y+math.cos(a)*d,ramp(0.9-abs(d)/4))
        c.outline(1); fr.append(c)
    return fr
add('FEATHER16',16,16,mk_feather())

def mk_seed():
    c=Canvas(8,8); disc(c,4,4,2.6,hl=True); c.outline(1); return [c]
add('SEED8',8,8,mk_seed())

def mk_crescent():
    fr=[]
    for f in range(1):
        c=Canvas(32,32)
        for y in range(32):
            for x in range(32):
                d1=math.hypot(x+0.5-14,y+0.5-16); d2=math.hypot(x+0.5-9-f,y+0.5-16)
                if d1<13 and d2>12: c.putmax(x,y,ramp(0.4+0.6*(1-abs(d1-12)/6)))
        c.outline(1); fr.append(c)
    return fr
add('CRESCENT32',32,32,mk_crescent())

def mk_spikes():
    c=Canvas(16,16)
    for (cx,cy) in ((4,6),(11,5),(7,11),(13,12)):
        poly(c,[(cx,cy-3),(cx+2,cy+2),(cx-2,cy+2)],lambda px,py:ramp(0.9-(py-cy+3)/7))
    c.outline(1); return [c]
add('SPIKES16',16,16,mk_spikes())

def mk_web():
    c=Canvas(32,32)
    for k in range(8):
        a=k*math.pi/4; line(c,16,16,16+math.cos(a)*15,16+math.sin(a)*15,13)
    for r in (5,10,14): ring(c,16,16,r,0.6,11)
    return [c]

def mk_shield():
    fr=[]
    for f in range(1):
        c=Canvas(32,32); pts=[(16+math.cos(k*math.pi/3+math.pi/6)*14,16+math.sin(k*math.pi/3+math.pi/6)*14) for k in range(6)]
        for i in range(6): line(c,*pts[i],*pts[(i+1)%6],15 if f==0 else 12,w=1.5)
        poly(c,pts,lambda px,py:4+int(3*abs(math.sin((px+py)*0.25+f))))
        fr.append(c)
    return fr
add('SHIELD32',32,32,mk_shield())

def mk_aura():
    fr=[]
    for f in range(2):
        c=Canvas(32,32)
        for k in range(7):
            x=4+k*4+random.uniform(-1,1); h=random.uniform(8,18)
            y0=30-((f*6+k*5)%14)
            for i in range(int(h)):
                c.putmax(x+math.sin((y0-i)*0.5)*0.8,y0-i,ramp(0.9-i/h))
        fr.append(c)
    return fr
add('AURA32',32,32,mk_aura()[:1])  # 1 frame: animated by flicker/scale (saves VRAM)

def mk_arrow(up):
    c=Canvas(8,8)
    pts=[(4,0.5),(7.5,4),(5,4),(5,7.5),(3,7.5),(3,4),(0.5,4)] if up else [(4,7.5),(7.5,4),(5,4),(5,0.5),(3,0.5),(3,4),(0.5,4)]
    poly(c,pts,lambda px,py:ramp(0.9)); c.outline(1); return [c]
add('ARROWUP8',8,8,mk_arrow(True))
add('ARROWDN8',8,8,mk_arrow(False))

def mk_plus():
    fr=[]
    for f in range(2):
        c=Canvas(8,8); s=3 if f==0 else 2
        line(c,4-s,4,4+s,4,14,w=1.5); line(c,4,4-s,4,4+s,14,w=1.5); c.outline(1); fr.append(c)
    return fr
add('PLUS8',8,8,mk_plus())

def mk_z():
    c=Canvas(8,8); line(c,1,1,6,1,13); line(c,6,1,1,6,13); line(c,1,6,6,6,13); c.outline(1); return [c]
add('Z8',8,8,mk_z())

def mk_dizzy():
    fr=[]
    for f in range(2):
        c=Canvas(8,8); pts=[]
        for k in range(10):
            a=-math.pi/2+k*math.pi/5+f*0.6; r=3.6 if k%2==0 else 1.6
            pts.append((4+math.cos(a)*r,4+math.sin(a)*r))
        poly(c,pts,lambda px,py:ramp(0.95)); c.outline(1); fr.append(c)
    return fr
add('DIZZY8',8,8,mk_dizzy())

def mk_skull():
    c=Canvas(16,16)
    disc(c,8,7,5.5,hl=False,core=0.9)
    for x in range(5,12): c.putmax(x,12,ramp(0.8)); c.putmax(x,13,ramp(0.7))
    for (x,y) in ((6,7),(10,7)):
        for dx in (0,1):
            for dy in (0,1): c.p[y+dy][x+dx-1]=1
    c.p[10][8]=1
    c.outline(1); return [c]
add('SKULL16',16,16,mk_skull())

def mk_mud():
    fr=[]
    for f in range(2):
        c=Canvas(16,16)
        disc(c,8,8,4.5+f,core=0.85)
        for k in range(5):
            a=k*1.25+f; disc(c,8+math.cos(a)*(5+f*1.5),8+math.sin(a)*(5+f*1.5),1.6,hl=False,core=0.7)
        c.outline(1); fr.append(c)
    return fr
add('MUD16',16,16,mk_mud())

def mk_grain():
    fr=[]
    for f in range(2):
        c=Canvas(8,8)
        for k in range(3): c.putmax(random.randint(1,6),random.randint(1,6),ramp(random.uniform(0.5,0.9)))
        fr.append(c)
    return fr
add('GRAIN8',8,8,mk_grain())

def mk_hail():
    c=Canvas(8,8); disc(c,4,4,2.4,hl=True); c.outline(1); return [c]
add('HAIL8',8,8,mk_hail())

def mk_rain():
    c=Canvas(8,8); line(c,5,0,2,7,11); c.putmax(2,7,14); return [c]
add('RAIN8',8,8,mk_rain())

def mk_sunray():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); disc(c,8,8,4,hl=False)
        for k in range(8):
            a=k*math.pi/4+f*math.pi/8; line(c,8+math.cos(a)*5,8+math.sin(a)*5,8+math.cos(a)*7.5,8+math.sin(a)*7.5,12)
        fr.append(c)
    return fr
add('SUNRAY16',16,16,mk_sunray())

def mk_target():
    fr=[]
    for f in range(2):
        c=Canvas(32,16)
        for y in range(16):
            for x in range(32):
                d=math.hypot((x+0.5-16)/15,(y+0.5-8)/7)
                if abs(d-0.92)<0.09: c.putmax(x,y,13 if f==0 else 9)
                elif abs(d-0.5)<0.08: c.putmax(x,y,9 if f==0 else 13)
        fr.append(c)
    return fr
add('TARGET32',32,16,mk_target())

def mk_fang():
    fr=[]
    for f in range(2):
        c=Canvas(32,32); gap=[9,2][f]
        for half in (0,1):
            y0 = 16-gap-6 if half==0 else 16+gap
            for k in range(4):
                x=6+k*6
                if half==0: poly(c,[(x-3,y0),(x+3,y0),(x,y0+7)],lambda px,py:ramp(0.95-(py-y0)/10))
                else: poly(c,[(x-3,y0+7),(x+3,y0+7),(x,y0)],lambda px,py:ramp(0.95-(y0+7-py)/10))
            for x in range(3,30):
                c.putmax(x,y0 if half==0 else y0+7,ramp(0.6))
        c.outline(1); fr.append(c)
    return fr
add('FANG32',32,32,mk_fang())

def mk_horn():
    fr=[]
    for f in range(2):
        c=Canvas(16,16)
        poly(c,[(1,4),(1,12),(15,8)],lambda px,py:ramp(0.95-abs(py-8)/6 - (0.15 if int(px+f*2)%4<2 else 0)))
        c.outline(1); fr.append(c)
    return fr
add('HORN16',16,16,mk_horn())

def mk_glow():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); disc(c,8,8,7.5-f,hl=False,falloff=0.6); fr.append(c)
    return fr
add('GLOW16',16,16,mk_glow())

def mk_cross():
    c=Canvas(16,16); line(c,2,2,13,13,12,w=2.5); line(c,13,2,2,13,12,w=2.5); line(c,3,3,12,12,15); line(c,12,3,3,12,15); c.outline(1); return [c]
add('CROSS16',16,16,mk_cross())

def mk_iceblock():
    c=Canvas(32,32)
    pts=[(5,4),(27,2),(30,26),(16,31),(2,27)]
    poly(c,pts,lambda px,py:ramp(0.35+0.4*((px-py+32)/64)+(0.3 if abs(px-py-2)<1.5 else 0)))
    for i in range(5): line(c,*pts[i],*pts[(i+1)%5],15)
    return [c]
add('ICEBLOCK32',32,32,mk_iceblock())

def mk_exclaim():
    c=Canvas(8,8); line(c,4,0,4,4,14,w=1.6); c.putmax(4,6,14); c.putmax(3,6,14); c.outline(1); return [c]
add('EXCLAIM8',8,8,mk_exclaim())

def mk_petal():
    fr=[]
    for f in range(4):
        c=Canvas(8,8); a=f*math.pi/4
        for y in range(8):
            for x in range(8):
                dx=x+0.5-4; dy=y+0.5-4
                u=dx*math.cos(a)+dy*math.sin(a); v=-dx*math.sin(a)+dy*math.cos(a)
                if (u/3.5)**2+(v/1.8)**2<1: c.putmax(x,y,ramp(0.9-abs(v)/3))
        c.outline(1); fr.append(c)
    return fr
add('PETAL8',8,8,mk_petal())

def mk_tri():
    c=Canvas(16,16); poly(c,[(8,1),(15,14),(1,14)],lambda px,py:ramp(0.95-math.hypot(px-8,py-9)/9)); c.outline(1); return [c]
add('TRI16',16,16,mk_tri())

def mk_coin():
    fr=[]
    for f in range(2):
        c=Canvas(8,8); w=3.2 if f==0 else 1.4
        for y in range(8):
            for x in range(8):
                if ((x+0.5-4)/w)**2+((y+0.5-4)/3.2)**2<1: c.putmax(x,y,ramp(0.9-0.3*(x+y)/8))
        c.outline(1); fr.append(c)
    return fr
add('COIN8',8,8,mk_coin())

def mk_eye():
    fr=[]
    for f in range(2):
        c=Canvas(16,16); op=1.0 if f==0 else 0.5
        for y in range(16):
            for x in range(16):
                X=(x+0.5-8)/7.5; Y=(y+0.5-8)/(4.5*op)
                if abs(Y)<(1-X*X) : c.putmax(x,y,ramp(0.5))
        disc(c,8,8,2.6*op+0.4,hl=False); c.putmax(8,8,1); c.outline(1); fr.append(c)
    return fr
add('EYE16',16,16,mk_eye())

def mk_doll():
    c=Canvas(16,16)
    disc(c,8,5,4,hl=True,core=0.8); disc(c,8,11,4.5,hl=True,core=0.7)
    c.p[4][6]=1; c.p[4][10]=1; c.p[6][8]=1
    for (x,y) in ((3,10),(13,10)): disc(c,x,y,1.5,hl=False,core=0.6)
    c.outline(1); return [c]
add('DOLL16',16,16,mk_doll())

def mk_cloud():
    fr=[]
    for f in range(2):
        c=Canvas(32,32)
        for k in range(7):
            a=k*0.9+f*0.5; d=6+3*math.sin(k*2.1)
            disc(c,16+math.cos(a)*d,16+math.sin(a)*d*0.8,6+math.sin(k)*1.5,core=0.7)
        fr.append(c)
    return fr

def mk_moon():
    c=Canvas(16,16)
    for y in range(16):
        for x in range(16):
            if math.hypot(x+0.5-8,y+0.5-8)<6.5 and math.hypot(x+0.5-11,y+0.5-6)>5.5: c.putmax(x,y,ramp(0.9-0.3*y/16))
    c.outline(1); return [c]
add('MOON16',16,16,mk_moon())

def mk_drill():
    fr=[]
    for f in range(3):
        c=Canvas(16,16)
        for x in range(1,16):
            w=(16-x)*0.35
            for y in range(16):
                if abs(y+0.5-8)<w:
                    t=0.6+0.4*math.sin((x*1.2-(y-8)*0.8)+f*2.1)
                    c.putmax(x,y,ramp(t))
        c.outline(1); fr.append(c)
    return fr
add('DRILL16',16,16,mk_drill())

def mk_column():
    fr=[]
    for f in range(2):
        c=Canvas(16,32)
        for y in range(32):
            w=5+1.5*math.sin(y*0.5+f*2)
            for x in range(16):
                d=abs(x+0.5-8)
                if d<w: c.putmax(x,y,ramp(0.95-d/w*0.6-(0.15 if (y+f*3)%6<2 else 0)))
        c.outline(1); fr.append(c)
    return fr
add('COLUMN16',16,32,mk_column())

# ---- UI shapes (fixed palette indices)
# UI palette: 0 transp,1 black,2 dgray,3 gray,4 white,5 red,6 dred,7 green,8 dgreen,9 yellow,10 dyellow,11 blue,12 dblue,13 orange,14 lgray,15 shadow
def mk_shadow(w,h):
    c=Canvas(w,h)
    for y in range(h):
        for x in range(w):
            # dithered (checkerboard) so it reads as a soft shadow without hardware blending
            if ((x+0.5-w/2)/(w/2-0.5))**2+((y+0.5-h/2)/(h/2-0.5))**2<=1 and (x+y)%2==0: c.p[y][x]=15
    return [c]
add('SHADOW16',16,8,mk_shadow(16,8),True)
add('SHADOW32',32,8,mk_shadow(32,8),True)
add('SHADOW32L',32,16,mk_shadow(32,16),True)

def mk_ball():
    fr=[]
    for f in range(4):
        c=Canvas(16,16); tilt=[0,0,-0.35,0.35][f]
        for y in range(16):
            for x in range(16):
                dx=x+0.5-8; dy=y+0.5-9
                u=dx*math.cos(tilt)+dy*math.sin(tilt); v=-dx*math.sin(tilt)+dy*math.cos(tilt)
                d=math.hypot(u,v)
                if d<6.2:
                    if f==1 and v<-0.5: col=5 if d<5.2 else 1  # open top lifted
                    if abs(v)<0.9: col=1
                    elif v<0: col=5 if d<5.4 else 1
                    else: col=4 if d<5.4 else 1
                    if f==1 and v<-0.5:
                        yy=y-3
                        if 0<=yy<16: c.p[yy][x]=col
                        continue
                    c.p[y][x]=col
        if f!=1:
            for (x,y) in ((7,8),(8,8),(7,9),(8,9)): c.p[y][x]=4
            c.p[8][6]=1; c.p[8][9]=1
        fr.append(c)
    return fr
add('BALL16',16,16,mk_ball(),True)

def mk_reticle():
    c=Canvas(16,16)
    for y in range(16):
        for x in range(16):
            d=math.hypot(x+0.5-8,y+0.5-8)
            if abs(d-6)<0.8: c.p[y][x]=4
    for k in (0,1,14,15):
        c.p[8][k]=5; c.p[k][8]=5; c.p[7][k]=5 if k in (0,15) else c.p[7][k]
    c.p[8][8]=5
    return [c]
add('RETICLE16',16,16,mk_reticle(),True)

def mk_aimdot():
    c=Canvas(8,8)
    for y in range(8):
        for x in range(8):
            d=math.hypot(x+0.5-4,y+0.5-4)
            if d<2.0: c.p[y][x]=4
            elif d<3.0: c.p[y][x]=1
    return [c]
add('AIMDOT8',8,8,mk_aimdot(),True)

# 5x7 pixel font for numbers/words (ramp: outline 1, fill 15)
FONT = {
'0':["01110","10001","10011","10101","11001","10001","01110"],
'1':["00100","01100","00100","00100","00100","00100","01110"],
'2':["01110","10001","00001","00110","01000","10000","11111"],
'3':["11110","00001","00001","01110","00001","00001","11110"],
'4':["00010","00110","01010","10010","11111","00010","00010"],
'5':["11111","10000","11110","00001","00001","10001","01110"],
'6':["00110","01000","10000","11110","10001","10001","01110"],
'7':["11111","00001","00010","00100","01000","01000","01000"],
'8':["01110","10001","10001","01110","10001","10001","01110"],
'9':["01110","10001","10001","01111","00001","00010","01100"],
'!':["00100","00100","00100","00100","00100","00000","00100"],
'+':["00000","00100","00100","11111","00100","00100","00000"],
'-':["00000","00000","00000","11111","00000","00000","00000"],
'A':["01110","10001","10001","11111","10001","10001","10001"],
'B':["11110","10001","10001","11110","10001","10001","11110"],
'C':["01110","10001","10000","10000","10000","10001","01110"],
'D':["11110","10001","10001","10001","10001","10001","11110"],
'E':["11111","10000","10000","11110","10000","10000","11111"],
'F':["11111","10000","10000","11110","10000","10000","10000"],
'G':["01110","10001","10000","10111","10001","10001","01111"],
'H':["10001","10001","10001","11111","10001","10001","10001"],
'I':["01110","00100","00100","00100","00100","00100","01110"],
'K':["10001","10010","10100","11000","10100","10010","10001"],
'L':["10000","10000","10000","10000","10000","10000","11111"],
'M':["10001","11011","10101","10101","10001","10001","10001"],
'N':["10001","11001","10101","10011","10001","10001","10001"],
'O':["01110","10001","10001","10001","10001","10001","01110"],
'P':["11110","10001","10001","11110","10000","10000","10000"],
'R':["11110","10001","10001","11110","10100","10010","10001"],
'S':["01111","10000","10000","01110","00001","00001","11110"],
'T':["11111","00100","00100","00100","00100","00100","00100"],
'U':["10001","10001","10001","10001","10001","10001","01110"],
'V':["10001","10001","10001","10001","10001","01010","00100"],
'W':["10001","10001","10001","10101","10101","10101","01010"],
'X':["10001","10001","01010","00100","01010","10001","10001"],
'Y':["10001","10001","01010","00100","00100","00100","00100"],
' ':["00000"]*7,
}
def text_canvas(s,w,h):
    c=Canvas(w,h); sp=6 if len(s)*6-1<=w-2 else 5; tw=len(s)*sp-1; x0=(w-tw)//2; y0=(h-7)//2
    for i,ch in enumerate(s):
        g=FONT[ch]
        for y in range(7):
            for x in range(5):
                if g[y][x]=='1' and 0<=x0+i*sp+x<w: c.p[y0+y][x0+i*sp+x]=15 if y<4 else 12
    c.outline(1)
    return c
DIGITS=[text_canvas(ch,8,8) for ch in '0123456789!+-']
add('DIGITS',8,8,DIGITS)
WORDS=['CRIT','SUPER','WEAK','MISS','BLOCK','IMMUNE','LV UP','MAX','RESIST']
for w in WORDS:
    add('W_'+w.replace(' ','_'),32,8,[text_canvas(w,32,8)])

# ---------------------------------------------------------------- palettes
def lerp(a,b,t): return tuple(a[i]+(b[i]-a[i])*t for i in range(3))
def mkpal(keys):
    # keys: list of (pos 0..1, (r,g,b) 0..255)
    pal=[(0,0,0)]
    for i in range(1,16):
        t=(i-1)/14
        for k in range(len(keys)-1):
            if keys[k][0]<=t<=keys[k+1][0]:
                u=(t-keys[k][0])/(keys[k+1][0]-keys[k][0]+1e-9); col=lerp(keys[k][1],keys[k+1][1],u); break
        pal.append(col)
    return pal
P={}
def P3(name,dark,mid,light,top=(255,255,255)):
    P[name]=mkpal([(0,dark),(0.12,dark),(0.5,mid),(0.85,light),(1,top)])
P3('NORMAL',(60,56,52),(170,165,150),(230,226,210))
P3('FIGHTING',(70,20,10),(190,70,40),(250,170,110))
P3('FLYING',(40,50,90),(140,160,230),(220,230,255))
P3('POISON',(50,10,60),(160,60,190),(230,160,240))
P3('GROUND',(60,40,20),(180,140,80),(235,210,150))
P3('ROCK',(50,40,30),(150,130,90),(210,195,160),(240,230,210))
P3('BUG',(30,50,10),(140,180,40),(210,235,120))
P3('GHOST',(25,15,45),(110,80,170),(190,160,240))
P3('STEEL',(40,45,55),(150,160,180),(220,225,235))
P3('MYSTERY',(30,30,30),(120,120,140),(200,200,220))
P3('FIRE',(90,10,0),(240,90,20),(255,210,60),(255,255,210))
P3('WATER',(10,30,90),(50,120,230),(140,210,255),(240,255,255))
P3('GRASS',(10,50,10),(60,170,50),(170,240,110),(240,255,220))
P3('ELECTRIC',(80,60,0),(240,200,30),(255,250,140),(255,255,255))
P3('PSYCHIC',(80,10,50),(240,80,160),(255,170,220))
P3('ICE',(30,70,110),(120,200,240),(210,245,255))
P3('DRAGON',(30,10,80),(110,60,220),(190,160,255))
P3('DARK',(15,10,10),(70,55,50),(150,130,120),(200,180,170))
P3('HEAL',(10,60,30),(90,220,130),(200,255,210))
P3('SLEEP',(20,20,70),(120,120,220),(210,210,255))
P3('HEART',(90,10,40),(240,90,150),(255,190,220))
P3('GOLD',(90,60,0),(230,180,40),(255,240,150))
P3('SAND',(80,60,30),(200,170,110),(240,225,180))
P3('MUD',(40,25,10),(110,80,45),(170,130,80))
P3('SMOKE',(40,40,45),(130,130,140),(210,210,215))
P3('WHITE',(90,90,110),(200,200,215),(245,245,255))
P3('SHADOW',(5,0,15),(50,20,70),(110,60,140),(170,120,200))
P3('BLOOD',(60,0,0),(170,20,30),(240,90,90))
P3('RAIN',(30,60,120),(90,140,210),(170,210,255))
P3('DIGIT_W',(20,20,20),(200,200,200),(250,250,250))
P3('DIGIT_Y',(40,30,0),(240,200,40),(255,250,160))
P3('DIGIT_G',(0,40,10),(60,210,90),(170,255,180))
P3('DIGIT_R',(50,0,0),(230,60,50),(255,170,150))
P3('DIGIT_B',(0,20,60),(70,140,240),(170,210,255))
PALS=list(P.keys())
UIPAL=[(0,0,0),(16,16,16),(70,70,80),(140,140,150),(250,250,250),(230,50,40),(130,20,20),(60,210,80),(20,110,40),(250,210,40),(160,120,10),(60,140,240),(20,60,140),(250,140,30),(200,200,210),(10,10,20)]

def c555(c): return (int(c[0])>>3)|((int(c[1])>>3)<<5)|((int(c[2])>>3)<<10)

# ---------------------------------------------------------------- arena props
# Real Emerald overworld object graphics (Rock Smash rock, Cut tree, Strength boulder, moving box),
# with their own palettes.
PROP_PALS={}
def prop_frames(png, frames, pal_name):
    im=Image.open(os.path.join(PE,'graphics/object_events/pics/misc',png))
    pal=im.getpalette()[:48]
    PROP_PALS[pal_name]=[tuple(pal[i*3:i*3+3]) for i in range(16)]
    out=[]
    for f in frames:
        c=Canvas(16,16)
        for y in range(16):
            for x in range(16):
                c.p[y][x]=im.getpixel((f*16+x,y)) & 15
        out.append(c)
    return out
add('PROP_ROCK',16,16,prop_frames('breakable_rock.png',[0,2,3],'PROP_ROCK'))
add('PROP_TREE',16,16,prop_frames('cuttable_tree.png',[0,2,3],'PROP_TREE'))
add('PROP_BOULDER',16,16,prop_frames('pushable_boulder.png',[0],'PROP_BOULDER'))
add('PROP_BOX',16,16,prop_frames('moving_box.png',[0],'PROP_BOX'))
for k,v in PROP_PALS.items():
    P[k]=v
PALS=list(P.keys())

# ---------------------------------------------------------------- new art (vfx)
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'fxart.py')).read())
PALS=list(P.keys())

# ---------------------------------------------------------------- emit
# Shapes in STATIC live in VRAM for the whole battle (UI, props, numbers, the most common particles);
# every other shape is streamed into a VRAM cache on first use (tile field has bit 15 set).
STATIC={'ORB8','ORB16','SPARK8','RING32','IMPACT32','SMOKE16','ARROWUP8','ARROWDN8','PLUS8','Z8','DIZZY8',
        'SHADOW16','SHADOW32','SHADOW32L','BALL16','RETICLE16','AIMDOT8','DIGITS','TARGET32','EXCLAIM8',
        'W_CRIT','W_SUPER','W_WEAK','W_MISS','W_BLOCK','W_IMMUNE','W_LV_UP','W_MAX','W_RESIST',
        'PROP_ROCK','PROP_TREE','PROP_BOULDER','PROP_BOX','EDGEDIM'}
atlas=bytearray(); dyn=bytearray(); table=[]
for (name,w,h,frames,ui) in SHAPES:
    shp,sz=SHAPES_SZ[(w,h)]
    tpf=(w//8)*(h//8)
    if name in STATIC:
        table.append((name,len(atlas)//32,(shp<<2)|sz,len(frames),tpf,w,h))
        for f in frames: atlas+=f.tiles()
    else:
        table.append((name,0x8000|(len(dyn)//32),(shp<<2)|sz,len(frames),tpf,w,h))
        for f in frames: dyn+=f.tiles()
ntiles=len(atlas)//32
maxdyn=max(len(fr)*(w//8)*(h//8) for (n,w,h,fr,ui) in SHAPES if n not in STATIC)
print('static tiles',ntiles,'dynamic tiles',len(dyn)//32,'largest dynamic shape',maxdyn,'shapes',len(SHAPES),'pals',len(PALS))
assert ntiles<=400, ntiles
assert len(dyn)//32 < 32768
os.makedirs(PE+'/graphics/rt',exist_ok=True)
open(PE+'/graphics/rt/fx_atlas.bin','wb').write(atlas)
open(PE+'/graphics/rt/fx_dyn.bin','wb').write(dyn)
with open(PE+'/include/rt_fx_ids.h','w') as o:
    o.write('// generated by fxgen.py\n#ifndef GUARD_RT_FX_IDS_H\n#define GUARD_RT_FX_IDS_H\n')
    o.write('#define FX_NONE 0\n')
    for i,(name,*_) in enumerate(table): o.write('#define FX_%s %d\n'%(name,i+1))
    o.write('#define FX_COUNT %d\n'%(len(table)+1))
    for i,n in enumerate(PALS): o.write('#define FXP_%s %d\n'%(n,i))
    o.write('#define FXP_UI 0xFF\n#define FXP_COUNT %d\n'%len(PALS))
    o.write('#define RT_FX_ATLAS_TILES %d\n'%ntiles)
    o.write('#endif\n')
with open(PE+'/src/data/rt/fx_data.h','w') as o:
    o.write('// generated by fxgen.py\nconst struct RtFxShape gRtFxShapes[] = {\n  {0,0,0,0,0,0},\n')
    for (name,tile,ss,nf,tpf,w,h) in table: o.write('  {%d,%d,%d,%d,%d,%d}, // %s\n'%(tile,ss,nf,tpf,w,h,name))
    o.write('};\nconst u16 gRtFxPalettes[][16] = {\n')
    for n in PALS: o.write('  {%s}, // %s\n'%(','.join('0x%04X'%c555(c) for c in P[n]),n))
    o.write('};\nconst u16 gRtUiPalette[16] = {%s};\n'%','.join('0x%04X'%c555(c) for c in UIPAL))
# preview
cols=16; S=3; prev=Image.new('RGB',(cols*34*S, ((sum(len(s[3]) for s in SHAPES)+cols-1)//cols)*34*S),(40,40,60))
k=0
for (name,w,h,frames,ui) in SHAPES:
    pal=UIPAL if ui else P['FIRE' if 'FLAME' in name else 'ELECTRIC' if 'BOLT' in name else 'GRASS' if 'LEAF' in name else 'WATER']
    for f in frames:
        im=Image.new('RGB',(w,h),(40,40,60))
        for y in range(h):
            for x in range(w):
                v=f.p[y][x]
                if v: im.putpixel((x,y),tuple(int(q) for q in pal[v]))
        prev.paste(im.resize((w*S,h*S),Image.NEAREST),((k%cols)*34*S,(k//cols)*34*S)); k+=1
prev.save(os.path.join(WORK, 'fx_preview.png'))
