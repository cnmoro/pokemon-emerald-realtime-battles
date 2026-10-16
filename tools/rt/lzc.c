// batch GBA LZ77 compressor: reads stdin records [u32 len][data], writes [u32 len][lzdata]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int comp(const uint8_t*d,int n,uint8_t*o){int p=0;o[p++]=0x10;o[p++]=n;o[p++]=n>>8;o[p++]=n>>16;int i=0;
 while(i<n){int fp=p++;uint8_t fl=0;for(int b=0;b<8&&i<n;b++){int bl=0,bd=0;int maxl=n-i<18?n-i:18;
  if(maxl>=3){for(int j=i-2;j>=0&&j>=i-4096;j--){if(d[j]!=d[i])continue;int l=1;while(l<maxl&&d[j+l]==d[i+l])l++;if(l>bl){bl=l;bd=i-j;if(l==maxl)break;}}}
  if(bl>=3){fl|=0x80>>b;int dd=bd-1;o[p++]=((bl-3)<<4)|((dd>>8)&15);o[p++]=dd&255;i+=bl;}else o[p++]=d[i++];}
  o[fp]=fl;}
 while(p&3)o[p++]=0;return p;}
int main(){uint32_t n;static uint8_t in[1<<20],out[1<<21];while(fread(&n,4,1,stdin)==1){fread(in,1,n,stdin);uint32_t m=comp(in,n,out);fwrite(&m,4,1,stdout);fwrite(out,1,m,stdout);}return 0;}
