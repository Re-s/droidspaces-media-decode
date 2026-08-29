/* 复现驱动的调用节奏：送 1 单元 → 等帧（100ms 超时）→ 再送
 * 与 nodrain 的"紧密连续送料"对照，判定问题是否出在节奏上。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "v4l2_backend.h"
int main(int argc,char**argv){
    int n=atoi(argv[2]);
    int pattern=(argc>3)?atoi(argv[3]):0;  /* 0=紧密 1=送一等一 */
    FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t*d=malloc(sz);fread(d,1,sz,f);fclose(f);
    long offs[64],lens[64];int nu=0;long i=0;
    while(i<sz&&nu<64){uint8_t h=d[i];int t=(h>>3)&0xF,hs=(h>>1)&1;long j=i+1;
        if((h>>2)&1)j++;long s2=0;
        if(hs){int sh=0;while(j<sz){uint8_t b=d[j++];s2|=(long)(b&0x7F)<<sh;sh+=7;if(!(b&0x80))break;}}
        else s2=sz-j;
        long e=j+s2;if(e>sz)break;
        if(t==2){offs[nu]=i;lens[nu]=e-i;nu++;}
        else if(nu>0)lens[nu-1]=e-offs[nu-1];
        i=e;}
    if(n<nu)nu=n;
    struct dmd_v4l2_dec dec;
    if(dmd_v4l2_open(&dec,4,1920,1080)<0){printf("open 失败\n");return 1;}
    int sent=0,got=0;
    for(int k=0;k<nu;k++){
        while(dmd_v4l2_send(&dec,d+offs[k],lens[k],(uint64_t)(k+1)*1000)==1){
            uint8_t*fd;size_t fl;uint64_t fp;int fi;
            if(dmd_v4l2_recv(&dec,&fd,&fl,&fp,&fi,50)==1){got++;dmd_v4l2_release(&dec,fi);}
        }
        sent++;
        if(pattern==1){  /* 模拟驱动：每送一个就尝试收一次 */
            uint8_t*fd;size_t fl;uint64_t fp;int fi;
            int r=dmd_v4l2_recv(&dec,&fd,&fl,&fp,&fi,100);
            if(r==1){got++;dmd_v4l2_release(&dec,fi);}
        }
    }
    for(int loop=0;loop<40;loop++){
        uint8_t*fd;size_t fl;uint64_t fp;int fi;
        int r=dmd_v4l2_recv(&dec,&fd,&fl,&fp,&fi,100);
        if(r==1){got++;dmd_v4l2_release(&dec,fi);}
        else if(r==2)break;
    }
    printf("节奏=%s 送 %d 单元, 收 %d 帧\n",pattern?"送一等一":"紧密",sent,got);
    dmd_v4l2_close(&dec);return 0;}
