#include <mgba/flags.h>
// Headless mGBA runner: run.c ROM SCRIPT
// script cmds: wait N | hold KEYS N | tap KEYS | shot FILE | savest FILE | loadst FILE | peek ADDR LEN | poke32 ADDR VAL | turbo N (A-mash)
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>
#include <mgba-util/image.h>
#include <png.h>
#include <stdio.h>
#include <stdarg.h>
#include <mgba/core/log.h>
#include <stdlib.h>
#include <string.h>
static struct mCore* core; static uint32_t* buf; static unsigned W,H;
static int keymask(const char* s){int m=0;for(;*s;s++){switch(*s){case 'A':m|=1;break;case 'B':m|=2;break;case 's':m|=4;break;case 'S':m|=8;break;case 'r':m|=16;break;case 'l':m|=32;break;case 'u':m|=64;break;case 'd':m|=128;break;case 'R':m|=256;break;case 'L':m|=512;break;}}return m;}
static void frames(int n,int k){for(int i=0;i<n;i++){core->setKeys(core,k);core->runFrame(core);}}
static void shot(const char*f){FILE*fp=fopen(f,"wb");png_structp p=png_create_write_struct(PNG_LIBPNG_VER_STRING,0,0,0);png_infop in=png_create_info_struct(p);png_init_io(p,fp);
 png_set_IHDR(p,in,W*2,H*2,8,PNG_COLOR_TYPE_RGB,0,0,0);png_write_info(p,in);unsigned char*row=malloc(W*6);
 for(unsigned y=0;y<H*2;y++){for(unsigned x=0;x<W*2;x++){uint32_t c=buf[(y/2)*W+x/2];row[x*3]=c&0xFF;row[x*3+1]=(c>>8)&0xFF;row[x*3+2]=(c>>16)&0xFF;}png_write_row(p,row);}
 png_write_end(p,0);fclose(fp);free(row);}
static void nolog(struct mLogger*l,int c,enum mLogLevel lv,const char*f,va_list a){}
static struct mLogger quiet={.log=nolog};
int main(int argc,char**argv){mLogSetDefaultLogger(&quiet);core=mCoreFind(argv[1]);core->init(core);mCoreInitConfig(core,0);
 mCoreLoadFile(core,argv[1]);W=240;H=160;buf=calloc(256*256,4);core->setVideoBuffer(core,buf,W);core->reset(core);
 FILE*sc=fopen(argv[2],"r");char line[512];
 while(fgets(line,sizeof line,sc)){char c[32],a[256],b[64];int n=sscanf(line,"%31s %255s %63s",c,a,b);if(n<1||c[0]=='#')continue;
  if(!strcmp(c,"wait"))frames(atoi(a),0);
  else if(!strcmp(c,"hold"))frames(atoi(b),keymask(a));
  else if(!strcmp(c,"tap")){frames(4,keymask(a));frames(8,0);}
  else if(!strcmp(c,"mash")){int k=n>2?keymask(b):1;for(int i=0;i<atoi(a);i++){frames(3,k);frames(5,0);}}
  else if(!strcmp(c,"shot"))shot(a);
  else if(!strcmp(c,"savest")){struct VFile*vf=VFileOpen(a,O_CREAT|O_TRUNC|O_RDWR);mCoreSaveStateNamed(core,vf,SAVESTATE_SAVEDATA);vf->close(vf);}
  else if(!strcmp(c,"loadst")){struct VFile*vf=VFileOpen(a,O_RDONLY);mCoreLoadStateNamed(core,vf,SAVESTATE_SAVEDATA);vf->close(vf);}
  else if(!strcmp(c,"peek")){uint32_t ad=strtoul(a,0,16);int len=atoi(b);printf("peek %08x:",ad);for(int i=0;i<len;i++)printf(" %02x",core->busRead8(core,ad+i));printf("\n");}
  else if(!strcmp(c,"peekp")){char d[64];sscanf(line,"%*s %*s %*s %63s",d);uint32_t p=core->busRead32(core,strtoul(a,0,16));uint32_t ad=p+strtoul(b,0,16);int len=atoi(d);printf("peekp %08x:",ad);for(int i=0;i<len;i++)printf(" %02x",core->busRead8(core,ad+i));printf("\n");}
  else if(!strcmp(c,"waitp")){char d[64],e[64];sscanf(line,"%*s %*s %*s %63s %63s",d,e);uint32_t off=strtoul(b,0,16);int val=strtol(d,0,16),mx=atoi(e),i;
    for(i=0;i<mx;i++){uint32_t p=core->busRead32(core,strtoul(a,0,16));if(p&&core->busRead8(core,p+off)==val)break;frames(1,0);}printf("waitp %d\n",i);}
  else if(!strcmp(c,"pokep16")){char d[64];sscanf(line,"%*s %*s %*s %63s",d);uint32_t p=core->busRead32(core,strtoul(a,0,16));core->busWrite16(core,p+strtoul(b,0,16),strtoul(d,0,16));}
  else if(!strcmp(c,"dump")){char d[64];sscanf(line,"%*s %*s %*s %63s",d);uint32_t ad=strtoul(a,0,16);int len=strtol(b,0,0);FILE*f=fopen(d,"wb");for(int i=0;i<len;i++)fputc(core->busRead8(core,ad+i),f);fclose(f);}
  else if(!strcmp(c,"poke8")){core->busWrite8(core,strtoul(a,0,16),strtoul(b,0,16));}
  else if(!strcmp(c,"poke16")){core->busWrite16(core,strtoul(a,0,16),strtoul(b,0,16));}
  else if(!strcmp(c,"poke32")){core->busWrite32(core,strtoul(a,0,16),strtoul(b,0,16));}
 }
 fflush(stdout);return 0;}
