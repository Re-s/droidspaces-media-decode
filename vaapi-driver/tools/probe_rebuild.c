/* 验证：flush 之后重建会话，能否从流中途继续正确解码？
 *
 * 这是"驱动侧透明重建"方案的前提。关键问题是重建后的新 MediaCodec
 * 没有参考帧，必须等到下一个 IDR 才能出正确画面 —— 如果中途重建只能
 * 靠 IDR，那这个方案在浏览器场景（长 GOP）就会有可见花屏。
 *
 * 做法：送 8 个 VCL（跨 IDR 之后进入 P/B），flush 收尾，
 * 然后重建会话、重送参数集 + 后续 VCL，看能否出帧。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmd_client.h"

static unsigned char *buf; static long buflen;

static int load(const char *p){
    FILE *f=fopen(p,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); buflen=ftell(f); fseek(f,0,SEEK_SET);
    buf=malloc(buflen);
    if(fread(buf,1,buflen,f)!=(size_t)buflen){fclose(f);return -1;}
    fclose(f); return 0;
}
static long next_sc(long from){
    for(long i=from;i+3<buflen;i++){
        if(buf[i]==0&&buf[i+1]==0&&buf[i+2]==1) return i;
        if(buf[i]==0&&buf[i+1]==0&&buf[i+2]==0&&buf[i+3]==1) return i;
    }
    return -1;
}

static struct dmd_session *mk(void){
    struct dmd_session_config cfg; dmd_session_config_init(&cfg);
    cfg.codec=DMD_CODEC_H264; cfg.width=1920; cfg.height=1080;
    struct dmd_error e; memset(&e,0,sizeof(e));
    struct dmd_session *s=dmd_session_create(&cfg,&e);
    if(!s) fprintf(stderr,"建会话失败 code=%d %s\n",e.code,e.msg);
    return s;
}

int main(int argc,char**argv){
    const char *path=argc>1?argv[1]:"/root/decode-test/test1080.h264";
    if(load(path)<0){fprintf(stderr,"读不了\n");return 1;}

    /* 记住参数集，重建时要重送 */
    long sps_off=-1,sps_len=0,pps_off=-1,pps_len=0;

    struct dmd_session *s=mk(); if(!s) return 1;
    int sent=0,got=0;
    long off=next_sc(0);
    long resume_from=-1;

    while(off>=0){
        long end=next_sc(off+3);
        long len=(end<0?buflen:end)-off;
        long sc=(buf[off+2]==1)?3:4;
        int nut=buf[off+sc]&0x1f;
        if(nut==7){sps_off=off;sps_len=len;}
        if(nut==8){pps_off=off;pps_len=len;}

        dmd_session_send_unit(s,buf+off,len);
        if(nut==1||nut==5) sent++;

        struct dmd_frame f; memset(&f,0,sizeof(f));
        if(dmd_session_next_frame(s,&f,300)==DMD_OK){got++;dmd_session_release_frame(s,&f);}

        if(sent>=8){ resume_from=(end<0?buflen:end); break; }
        off=end;
    }
    printf("阶段1: 送 %d VCL, 取 %d 帧\n",sent,got);

    /* flush 收尾 */
    dmd_session_finish_input(s);
    for(;;){
        struct dmd_frame f; memset(&f,0,sizeof(f));
        int rc=dmd_session_next_frame(s,&f,600);
        if(rc!=DMD_OK) break;
        got++; dmd_session_release_frame(s,&f);
    }
    printf("阶段1 flush 后累计 %d 帧\n",got);
    dmd_session_destroy(s);

    /* ==== 重建：从流中途继续 ==== */
    printf("\n--- 重建会话，从中途继续送 ---\n");
    s=mk(); if(!s) return 1;
    if(sps_off>=0) dmd_session_send_unit(s,buf+sps_off,sps_len);
    if(pps_off>=0) dmd_session_send_unit(s,buf+pps_off,pps_len);
    printf("已重送 SPS(%ld) PPS(%ld)\n",sps_len,pps_len);

    int sent2=0,got2=0,first_nut=-1;
    off=resume_from;
    while(off>=0&&sent2<12){
        long end=next_sc(off+3);
        long len=(end<0?buflen:end)-off;
        long sc=(buf[off+2]==1)?3:4;
        int nut=buf[off+sc]&0x1f;
        if(first_nut<0&&(nut==1||nut==5)) first_nut=nut;
        dmd_session_send_unit(s,buf+off,len);
        if(nut==1||nut==5) sent2++;
        struct dmd_frame f; memset(&f,0,sizeof(f));
        if(dmd_session_next_frame(s,&f,400)==DMD_OK){got2++;dmd_session_release_frame(s,&f);}
        off=end;
    }
    printf("续传首个 VCL 类型 = %d (1=非IDR, 5=IDR)\n",first_nut);
    printf("阶段2: 送 %d VCL, 取 %d 帧\n",sent2,got2);
    printf("\n=== 结论 ===\n%s\n",
        got2>0 ? "✓ 重建后能从中途继续出帧（不必等 IDR）"
               : "✗ 重建后中途无法出帧：必须等下一个 IDR，会有可见花屏");
    dmd_session_destroy(s);
    free(buf);
    return 0;
}
