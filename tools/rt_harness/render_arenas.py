import sys, numpy as np
from PIL import Image, ImageDraw
def render(i, rect):
    v=np.frombuffer(open('/tmp/ar/v%03d.bin'%i,'rb').read(),dtype=np.uint8)
    p=open('/tmp/ar/p%03d.bin'%i,'rb').read()
    pal=np.array([[ (c&31)*255//31, ((c>>5)&31)*255//31, ((c>>10)&31)*255//31] for c in (p[k*2]|p[k*2+1]<<8 for k in range(256))],dtype=np.uint8)
    # tiles: 1024 x 8x8 indices
    t=v[:0x8000].reshape(1024,8,4)
    tiles=np.zeros((1024,8,8),dtype=np.uint8); tiles[:,:,0::2]=t&15; tiles[:,:,1::2]=t>>4
    img=np.zeros((256,512,3),dtype=np.uint8); img[:]=pal[0]
    for sb in (28,26,24):
        for half in range(2):
            m=np.frombuffer(v[(sb+half)*0x800:(sb+half)*0x800+0x800].tobytes(),dtype='<u2').reshape(32,32)
            for ty in range(32):
                for tx in range(32):
                    e=int(m[ty,tx]); ti=e&0x3ff; tl=tiles[ti]
                    if e&0x400: tl=tl[:,::-1]
                    if e&0x800: tl=tl[::-1,:]
                    pl=(e>>12)*16
                    mask=tl!=0
                    blk=img[ty*8:ty*8+8,(half*32+tx)*8:(half*32+tx)*8+8]
                    blk[mask]=pal[pl+tl[mask]]
    if rect:
        x0,y0,x1,y1=rect
        dim=np.ones((256,512),bool); dim[max(0,y0):y1,max(0,x0):x1]=False
        img[dim]=(img[dim].astype(int)*9//16).astype(np.uint8)
    return Image.fromarray(img)
rects=[]
for l in open('/tmp/ar/rects.txt'):
    b=bytes.fromhex(l.split(':')[1].replace(' ','').strip())
    rects.append([int.from_bytes(b[k*2:k*2+2],'little',signed=True) for k in range(4)])
names=open('/tmp/warplist.txt').read().split()
for g in range(0,len(rects),6):
    W=Image.new('RGB',(512*3,(256+12)*2),(0,0,0)); d=ImageDraw.Draw(W)
    for k in range(6):
        i=g+k
        if i>=len(rects): break
        W.paste(render(i,rects[i]),((k%3)*512,(k//3)*268+12)); d.text(((k%3)*512+2,(k//3)*268),'%d %s'%(i,names[i][4:]),fill=(255,255,0))
    W.save('/tmp/ar/g%02d.png'%(g//6))
