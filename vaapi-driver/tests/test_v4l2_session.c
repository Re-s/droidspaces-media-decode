/* 直接用 dmd_client.h 的 session API 解码，复现驱动的调用序列。
 * 目的：把范围从"驱动 + session 层"压到"仅 session 层"。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dmd_client.h"
int main(int argc,char**argv){
    int n=atoi(argv[2]);
    /* argv[3]=whole：整条流一次 send_unit，用于验证送料粒度。
     * 上一轮定界发现：合成流按帧切分送料只出 1 帧，而源码流一个
     * temporal unit 含多帧。需要能测"多帧一单元"才能证实该假设。 */
    int whole = (argc>3 && argv[3][0]=='w');
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

    struct dmd_session_config cfg; dmd_session_config_init(&cfg);
    cfg.codec=4; cfg.width=1920; cfg.height=1080;
    struct dmd_error err; memset(&err,0,sizeof(err));
    struct dmd_session*s=dmd_session_create(&cfg,&err);
    if(!s){printf("create 失败: %s\n",err.msg);return 1;}

    int sent=0,got=0;
    if(whole){
        int rc=dmd_session_send_unit(s,d,sz);
        printf("整块送入 %ld 字节 rc=%d\n",sz,rc);
        if(rc==0){
            sent=1;
            for(int loop=0;loop<200;loop++){
                struct dmd_frame fr; memset(&fr,0,sizeof(fr));
                int r=dmd_session_next_frame(s,&fr,200);
                if(r==0){got++;dmd_session_release_frame(s,&fr);}
                else if(r==1)break;
            }
        }
        printf("session API: 送 %d 单元, 收 %d 帧\n",sent,got);
        dmd_session_destroy(s);return 0;
    }
    for(int k=0;k<nu;k++){
        int rc=dmd_session_send_unit(s,d+offs[k],lens[k]);
        if(rc!=0){printf("send_unit[%d] 失败 rc=%d: %s\n",k,rc,dmd_session_last_error(s));break;}
        sent++;
        /* 模拟驱动：每送一个尝试取一帧 */
        struct dmd_frame fr; memset(&fr,0,sizeof(fr));
        int fr_rc=dmd_session_next_frame(s,&fr,100);
        if(fr_rc==0){got++;dmd_session_release_frame(s,&fr);}
    }
    /* 收尾：继续取剩余帧 */
    for(int loop=0;loop<40;loop++){
        struct dmd_frame fr; memset(&fr,0,sizeof(fr));
        int rc=dmd_session_next_frame(s,&fr,200);
        if(rc==0){got++;dmd_session_release_frame(s,&fr);}
        else if(rc==1){printf("EOS\n");break;}
    }
    printf("session API: 送 %d 单元, 收 %d 帧\n",sent,got);
    dmd_session_destroy(s);return 0;}
