/*
 * probe_buffer_return —— 判定 OUTPUT 缓冲是否被归还
 *
 * 这是判断"解码会话有没有真正启动"最直接的探针，比等 SOURCE_CHANGE 更早
 * 一步：只要固件在跑，送进去的输入缓冲就会被消费并归还（DQBUF 成功）。
 *
 * 做法：填满 OUTPUT 队列后专门轮询 DQBUF，不做别的。
 *
 * 实测（第 87 轮，nabu 内核 4.14）：
 *   QBUF[0..3]  全部 ok（SPS/PPS/SEI/IDR）
 *   第 5 个 QBUF → EINVAL（队列满）
 *   5 次 poll(POLLOUT|POLLPRI|POLLIN, 1s) → 全部 revents=0
 *   5 次 DQBUF → 全部 EAGAIN
 * 缓冲有去无回。对照 doc/why-not-v4l2.md 的源码结论：会话未达
 * MSM_VIDC_START_DONE 时缓冲被标 DEFERRED 后丢弃，而 QBUF 仍返回 0。
 *
 * 判读：
 *   DQBUF 能成功归还  → 固件在跑，问题在更后面的协商步骤
 *   DQBUF 恒 EAGAIN   → 会话没启动，用户态再怎么调都没用
 *
 * 编译：cc -O2 -o probe_buffer_return probe_buffer_return.c
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

    /* 送 4 个填满队列，然后专门轮询 DQBUF 看有没有归还 */
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
    printf("  === 队列已满，轮询归还（每次 1s，共 5 次）===\n");
    for(int k=0;k<5;k++){
        struct pollfd pf={.fd=fd,.events=POLLOUT|POLLPRI|POLLIN};
        int pr=poll(&pf,1,1000);
        printf("    poll[%d] = %d revents=0x%04x", k, pr, pf.revents);
        if(pf.revents&POLLOUT) printf(" [OUT可写]");
        if(pf.revents&POLLPRI) printf(" [有事件]");
        if(pf.revents&POLLIN)  printf(" [CAP可读]");
        printf("\n");
        struct v4l2_buffer d; struct v4l2_plane dp[1];
        memset(&d,0,sizeof d);memset(dp,0,sizeof dp);
        d.type=f.type;d.memory=V4L2_MEMORY_USERPTR;d.m.planes=dp;d.length=1;
        int dr=ioctl(fd,VIDIOC_DQBUF,&d);
        printf("      DQBUF = %s\n", dr==0?"成功归还!":strerror(errno));
        if(dr==0) printf("      归还 index=%u bytesused=%u\n",d.index,dp[0].bytesused);
    }
    close(fd); free(data); return 0;
}
