/* * The conclusions of this probe** are not credible, and a record of lessons learned is retained.
 *
 * It reports that "rebuilding sessions can be transmitted from non-IDR frames", which leads to "rebuilding costs are acceptable."
 * But it** counted only frames, without checking the contents of the screen** -- those frames were all pure black (Y=16).
 *
 * Truth: Any action to discard the codec state (AmediaCodec_flush or re-establish session) will be destroyed
 * The reference frame, which must be relied on by the P/B frame of H.264. Decoding from non-IDR positions is restarted.
 * To blacken until the next IDR (one in every 30 frames of this test stream, so black at most 29 frames),
 * Showed as "A Vision Flash".
 *
 * The right approach is to be found in probe_replay.c: Replaying after reconstruction from the nearest IDR, and then replaying the project from the IDR.
 * Drops the redistributed frame. See probe_black.c for detection.
 *
 * Lessons: Verify decoding validity** can never be counted only in frame**. This error was made three times in this project.
 * (probe_rebuild, probe_drain, and the report "Browser full frame hardened")
 */
/* Verify: Can you correct the password from the stream by reconstructing the session after Flush?
 *
 * This is the premise of the "Driving Transparency Rebuilding". The key issue is the new MediaCodec after the reconstruction.
 * No frame of reference, wait until the next IDR is right -- if you can only recreate it in the middle of the road.
 * IDR, that's a plan with a visible screen on the browser scene.
 *
 * Practice: Send 8 VCLs (B/ P/ P after IDR), Flush closing,
 * Then recreate the session, re-transmit the argument set + follow-up VCL, see if you can get out of the frame.
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
    if(!s) fprintf(stderr,"Create session failed code=%d %s\n",e.code,e.msg);
    return s;
}

int main(int argc,char**argv){
    const char *path=argc>1?argv[1]:"/root/decode-test/test1080.h264";
    if(load(path)<0){fprintf(stderr,"I can’t read it.\n");return 1;}

    /* Remember the set of parameters. Reload when reconstructing. */
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
    printf("phase 1: Send %d VCL, Take %d frame\n",sent,got);

    /* Flush, close it. */
    dmd_session_finish_input(s);
    for(;;){
        struct dmd_frame f; memset(&f,0,sizeof(f));
        int rc=dmd_session_next_frame(s,&f,600);
        if(rc!=DMD_OK) break;
        got++; dmd_session_release_frame(s,&f);
    }
    printf("phase 1 flush Cumulative after %d frame\n",got);
    dmd_session_destroy(s);

    /* Rebuild the session and resume partway through the stream */
    printf("\n--- Rebuild session, continue from midway ---\n");
    s=mk(); if(!s) return 1;
    if(sps_off>=0) dmd_session_send_unit(s,buf+sps_off,sps_len);
    if(pps_off>=0) dmd_session_send_unit(s,buf+pps_off,pps_len);
    printf("Reloaded SPS(%ld) PPS(%ld)\n",sps_len,pps_len);

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
    printf("Continue First VCL Type = %d (1=Not IDR, 5=IDR)\n",first_nut);
    printf("phase 2: Send %d VCL, Take %d frame\n",sent2,got2);
    printf("\n=== Conclusions ===\n%s\n",
        got2>0 ? "✓ The reconstruction will continue from the middle. IDR)"
               : "✗ No frame can be drawn after reconstruction: we have to wait for the next one. IDR,There’s a visual screen.");
    dmd_session_destroy(s);
    free(buf);
    return 0;
}
