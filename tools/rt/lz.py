# GBA BIOS LZ77 (type 0x10) compressor, VRAM-safe (min disp 2)
def compress(data, vram_safe=True):
    n=len(data); out=bytearray([0x10, n&0xFF,(n>>8)&0xFF,(n>>16)&0xFF]); i=0
    mind = 2 if vram_safe else 1
    while i<n:
        flagpos=len(out); out.append(0); flags=0
        for b in range(8):
            if i>=n: break
            best_len=0; best_d=0
            start=max(0,i-4096)
            maxl=min(18,n-i)
            if maxl>=3:
                # search
                j=i-mind
                while j>=start:
                    if data[j]==data[i]:
                        l=1
                        while l<maxl and data[j+l]==data[i+l]: l+=1
                        if l>best_len:
                            best_len=l; best_d=i-j
                            if l==maxl: break
                    j-=1
            if best_len>=3:
                flags|=0x80>>b; d=best_d-1
                out.append(((best_len-3)<<4)|((d>>8)&0xF)); out.append(d&0xFF); i+=best_len
            else:
                out.append(data[i]); i+=1
        out[flagpos]=flags
    while len(out)%4: out.append(0)
    return bytes(out)
