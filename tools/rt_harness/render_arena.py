import sys, os
OUT=os.environ.get('OUT','/tmp/rt_out')
from PIL import Image
def render(vf,pf,out):
    v=open(vf,'rb').read(); p=open(pf,'rb').read()
    pal=[]
    for i in range(256):
        c=p[i*2]|p[i*2+1]<<8
        pal.append(((c&31)*255//31,((c>>5)&31)*255//31,((c>>10)&31)*255//31))
    W,H=512,256
    img=Image.new('RGB',(W,H),pal[0])
    px=img.load()
    for sb in (28,26,24):  # bottom, middle, top
        for ty in range(32):
            for tx in range(64):
                blk=sb+(tx//32)
                e=v[blk*0x800+(ty*32+tx%32)*2]|v[blk*0x800+(ty*32+tx%32)*2+1]<<8
                t=e&0x3ff; hf=(e>>10)&1; vf_=(e>>11)&1; pl=e>>12
                base=t*32
                for y in range(8):
                    for x in range(8):
                        sx=7-x if hf else x; sy=7-y if vf_ else y
                        b=v[base+sy*4+sx//2]
                        ci=(b>>4) if sx&1 else (b&15)
                        if ci: px[tx*8+x,ty*8+y]=pal[pl*16+ci]
    img.save(out)
for a in sys.argv[1:]:
    render(OUT+'/ar/v%s.bin'%a,OUT+'/ar/p%s.bin'%a,OUT+'/ar/full%s.png'%a)
