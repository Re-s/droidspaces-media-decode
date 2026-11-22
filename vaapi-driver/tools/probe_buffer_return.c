/*
 * Probe_buffer_return - Determines whether the OUTPUT buffer has been returned
 *
 * This is the most direct probe to determine whether the decoding session actually starts. It's earlier than waiting for SOURCE_CHANGE.
 * Step 1: As long as the solids are running, the input buffers sent in will be consumed and returned (DQBUF succeeded).
 *
 * Practice: Fill out the UTPUT queue and then specifically consult DQBUF, nothing more.
 *
 * Actual (87th Round, nbu kernel 4.14):
 *   QBUF [0.3] all OK (SPS/PPS/SEI/IDR)
 *   5 QBUF  ⁇  EINVAL (square full)
 *   5 times poll (POLLOUT ⁇ POLPLRI ⁇ POLLIN, 1s)  ⁇ Revents=0
 *   5 DQBUF *All EAGAIN
 * The buffer has no return. The source code conclusion for doc/why-not-v44l2.md is: the session is not reached
 * The buffer was thrown out by the label DEFERRED while the QBUF returned 0 when MSM_VIDC_START_DonE.
 *
 * Interpretation:
 *   DQBUF can be successfully returned... ..and the solids are running, and the problem is later in the negotiation process.
 *   The user-state is useless.
 *
 * cc-O2-o probe_buffer_returnprobe_buffer_return.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/videodev2.h>
static unsigned char*slurp(const char*p,size_t*n){
    FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END);*n=ftell(f);fseek(f,0,SEEK_SET);
    unsigned char*b=malloc(*n);
    if(fread(b,1,*n,f)!=*n){free(b);fclose(f);return NULL;}
    fclose(f);return b;
}
static size_t nsc(const unsigned char*d,size_t n,size_t f){
    for(size_t i=f;i+3<n;i++) if(!d[i]&&!d[i+1]&&d[i+2]==1) return i;
    return n;
}
int main(int argc,char**argv){
    size_t flen=0; unsigned char*data=slurp(argc>1?argv[1]:"/tmp/loc.h264",&flen);
    int fd=open("/dev/video32",O_RDWR|O_CLOEXEC);
    struct v4l2_format f; memset(&f,0,sizeof f);
    f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width=1280;f.fmt.pix_mp.height=720;
    f.fmt.pix_mp.pixelformat=v4l2_fourcc('H','2','6','4');
    f.fmt.pix_mp.num_planes=1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage=4194304;
    ioctl(fd,VIDIOC_S_FMT,&f);
    size_t isz=f.fmt.pix_mp.plane_fmt[0].sizeimage;
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count=4;rb.type=f.type;rb.memory=V4L2_MEMORY_USERPTR;
    ioctl(fd,VIDIOC_REQBUFS,&rb);
    struct v4l2_event_subscription sub; memset(&sub,0,sizeof sub);
    sub.type=V4L2_EVENT_SOURCE_CHANGE; ioctl(fd,VIDIOC_SUBSCRIBE_EVENT,&sub);
    unsigned t=f.type; ioctl(fd,VIDIOC_STREAMON,&t);
    void*ob[4]; for(int i=0;i<4;i++) ob[i]=aligned_alloc(4096,isz);

    /* Send 4 full queues and then ask DQBUF for return. */
    size_t pos=nsc(data,flen,0);
    for(int i=0;i<4;i++){
        size_t end=nsc(data,flen,pos+3), len=end-pos;
        memcpy(ob[i],data+pos,len);
        struct v4l2_buffer b; struct v4l2_plane p[1];
        memset(&b,0,sizeof b);memset(p,0,sizeof p);
        b.type=f.type;b.memory=V4L2_MEMORY_USERPTR;b.index=i;
        b.m.planes=p;b.length=1;
        p[0].m.userptr=(unsigned long)ob[i];p[0].length=isz;p[0].bytesused=len;
        printf("  QBUF[%d]=%s\n",i,ioctl(fd,VIDIOC_QBUF,&b)==0?"ok":strerror(errno));
        pos=end;
    }
    printf("  === Queues full, round returned (each time) 1s,Total 5 times)===\n");
    for(int k=0;k<5;k++){
        struct pollfd pf={.fd=fd,.events=POLLOUT|POLLPRI|POLLIN};
        int pr=poll(&pf,1,1000);
        printf("    poll[%d] = %d revents=0x%04x", k, pr, pf.revents);
        if(pf.revents&POLLOUT) printf(" [OUT Writeable]");
        if(pf.revents&POLLPRI) printf(" [There’s been an incident.]");
        if(pf.revents&POLLIN)  printf(" [CAP readable]");
        printf("\n");
        struct v4l2_buffer d; struct v4l2_plane dp[1];
        memset(&d,0,sizeof d);memset(dp,0,sizeof dp);
        d.type=f.type;d.memory=V4L2_MEMORY_USERPTR;d.m.planes=dp;d.length=1;
        int dr=ioctl(fd,VIDIOC_DQBUF,&d);
        printf("      DQBUF = %s\n", dr==0?"Successful return!":strerror(errno));
        if(dr==0) printf("      return index=%u bytesused=%u\n",d.index,dp[0].bytesused);
    }
    close(fd); free(data); return 0;
}
