/* Recreates the call sequence driven by dmd_client.h 's message API decoding.
 * Exercise the session layer independently of the VA-API driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dmd_client.h"
int main(int argc,char**argv){
    int n=atoi(argv[2]);
    /* Argv [3] =whole: whole strip flow send_unit for verification of grain size of the delivery material.
     * Last round of delimitation found: Synthetic stream distribution by frame cut out only 1 frame, and source stream one
     * The scenario is supported by a multi-frame frame. */
    int whole = (argc>3 && argv[3][0]=='w');
    /* Argv [3] = flame: press OBU_FRAME (one unit per frame) instead of TD.
     * A TU that has a lot of frames, press TD to plug them into a cell.
     * A distinction needs to be made between "unit=TU" and "module=one frame" for the delivery. */
    int perframe = (argc>3 && argv[3][0]=='f');
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
    if(!s){printf("create failed: %s\n",err.msg);return 1;}

    int sent=0,got=0;
    if(perframe){
        /* Scanning on OBU, whenever OBU_FRAME(6) meets, together with its predecessor TD/SEQ sent out as a unit. */
        long i=0, ustart=0; int nu2=0;
        while(i<sz){
            unsigned char h=d[i]; int t=(h>>3)&0xF; long j=i+1;
            long osz=0; int shf=0;
            if((h>>1)&1){ while(j<sz){ unsigned char b=d[j++]; osz|=(long)(b&0x7F)<<shf; shf+=7; if(!(b&0x80))break; } }
            else osz=sz-j;
            long e=j+osz; if(e>sz)break;
            if(t==6){
                int rc=dmd_session_send_unit(s,d+ustart,(size_t)(e-ustart));
                if(rc==0){
                    sent++; nu2++;
                    struct dmd_frame fr; memset(&fr,0,sizeof(fr));
                    if(dmd_session_next_frame(s,&fr,300)==0){got++;dmd_session_release_frame(s,&fr);}
                }
                ustart=e;
            }
            i=e;
        }
        for(int loop=0;loop<100;loop++){
            struct dmd_frame fr; memset(&fr,0,sizeof(fr));
            int r=dmd_session_next_frame(s,&fr,200);
            if(r==0){got++;dmd_session_release_frame(s,&fr);}
            else if(r==1)break;
        }
        printf("session API: Send %d Module, Take it. %d frame\n",sent,got);
        dmd_session_destroy(s);return 0;
    }
    if(whole){
        int rc=dmd_session_send_unit(s,d,sz);
        printf("Whole %ld bytes rc=%d\n",sz,rc);
        if(rc==0){
            sent=1;
            for(int loop=0;loop<200;loop++){
                struct dmd_frame fr; memset(&fr,0,sizeof(fr));
                int r=dmd_session_next_frame(s,&fr,200);
                if(r==0){got++;dmd_session_release_frame(s,&fr);}
                else if(r==1)break;
            }
        }
        printf("session API: Send %d Module, Take it. %d frame\n",sent,got);
        dmd_session_destroy(s);return 0;
    }
    for(int k=0;k<nu;k++){
        int rc=dmd_session_send_unit(s,d+offs[k],lens[k]);
        if(rc!=0){printf("send_unit[%d] failed rc=%d: %s\n",k,rc,dmd_session_last_error(s));break;}
        sent++;
        /* Simulate driver: each sent tries to get a frame */
        struct dmd_frame fr; memset(&fr,0,sizeof(fr));
        int fr_rc=dmd_session_next_frame(s,&fr,100);
        if(fr_rc==0){got++;dmd_session_release_frame(s,&fr);}
    }
    /* End of story: Continue with the remaining frame */
    for(int loop=0;loop<40;loop++){
        struct dmd_frame fr; memset(&fr,0,sizeof(fr));
        int rc=dmd_session_next_frame(s,&fr,200);
        if(rc==0){got++;dmd_session_release_frame(s,&fr);}
        else if(rc==1){printf("EOS\n");break;}
    }
    printf("session API: Send %d Module, Take it. %d frame\n",sent,got);
    dmd_session_destroy(s);return 0;}
