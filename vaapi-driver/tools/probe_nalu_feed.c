/*
 * probe_nalu_feed —— 按 Annex-B 真实 NALU 边界喂料，验证是否能触发 SOURCE_CHANGE
 *
 * 与 probe_device_support.c 的区别：那个按固定大小切块，这个按起始码切出
 * 完整 NALU。msm_vidc 是 stateful 解码器，理论上要求每个 OUTPUT buffer
 * 至少是一个完整 NALU —— 本探针用来排除"切块方式不对"这个可能。
 *
 * 实测（第 86 轮，nabu 内核 4.14）：
 *   NALU[0] type=7 (SPS)  30 字节    QBUF ok
 *   NALU[1] type=8 (PPS)   9 字节    QBUF ok
 *   NALU[2] type=6 (SEI) 690 字节    QBUF ok
 *   NALU[3] type=5 (IDR) 22857 字节  QBUF ok
 *   → 仍无 SOURCE_CHANGE
 * 所以喂料切分不是 nabu 解码路径起不来的原因。
 *
 * 编译：cc -O2 -o probe_nalu_feed probe_nalu_feed.c
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
/* 按 Annex-B 起始码切出真正的 NALU，而不是任意切块。
 * msm_vidc 是 stateful 解码器，要求每个 OUTPUT buffer 至少是完整 NALU。*/
static unsigned char *slurp(const char *p, size_t *n){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *b=malloc(*n);
    if (fread(b,1,*n,f)!=*n) { free(b); fclose(f); return NULL; }
    fclose(f); return b;
}
static size_t next_sc(const unsigned char*d,size_t n,size_t from){
    for(size_t i=from;i+3<n;i++)
        if(d[i]==0&&d[i+1]==0&&d[i+2]==1) return i;
    return n;
}
int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"/tmp/loc.h264";
    size_t flen=0; unsigned char*data=slurp(path,&flen);
    if(!data){printf("  读不到 %s\n",path);return 1;}
    printf("  文件 %zu 字节\n", flen);

    int fd=open("/dev/video32",O_RDWR|O_CLOEXEC);
    struct v4l2_format f; memset(&f,0,sizeof f);
    f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width=1280; f.fmt.pix_mp.height=720;
    f.fmt.pix_mp.pixelformat=v4l2_fourcc('H','2','6','4');
    f.fmt.pix_mp.num_planes=1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage=4194304;
    ioctl(fd,VIDIOC_S_FMT,&f);
    size_t isz=f.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count=4; rb.type=f.type; rb.memory=V4L2_MEMORY_USERPTR;
    if(ioctl(fd,VIDIOC_REQBUFS,&rb)){printf("  REQBUFS 失败\n");return 1;}

    struct v4l2_event_subscription sub; memset(&sub,0,sizeof sub);
    sub.type=V4L2_EVENT_SOURCE_CHANGE; ioctl(fd,VIDIOC_SUBSCRIBE_EVENT,&sub);
    unsigned t=f.type; ioctl(fd,VIDIOC_STREAMON,&t);

    void*bufs[4]; for(int i=0;i<4;i++) bufs[i]=aligned_alloc(4096,isz);

    /* 前 4 个 NALU 逐个送（含 SPS/PPS/IDR） */
    size_t pos=next_sc(data,flen,0);
    for(int i=0;i<4 && pos<flen;i++){
        size_t end=next_sc(data,flen,pos+3);
        size_t len=end-pos;
        unsigned nal_type = (pos+3<flen)? (data[pos+3]&0x1f) : 0;
        memcpy(bufs[i],data+pos,len);
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b,0,sizeof b); memset(pl,0,sizeof pl);
        b.type=f.type; b.memory=V4L2_MEMORY_USERPTR; b.index=i;
        b.m.planes=pl; b.length=1;
        pl[0].m.userptr=(unsigned long)bufs[i];
        pl[0].length=isz; pl[0].bytesused=len;
        int q=ioctl(fd,VIDIOC_QBUF,&b);
        printf("  NALU[%d] type=%u %zu 字节 QBUF=%s\n",i,nal_type,len,
               q==0?"ok":strerror(errno));
        struct pollfd pf={.fd=fd,.events=POLLPRI};
        poll(&pf,1,2000);
        if(pf.revents&POLLPRI){
            struct v4l2_event ev; memset(&ev,0,sizeof ev);
            ioctl(fd,VIDIOC_DQEVENT,&ev);
            printf("  ✓✓ SOURCE_CHANGE 在第 %d 个 NALU 后到达！\n",i);
            struct v4l2_format cf; memset(&cf,0,sizeof cf);
            cf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl(fd,VIDIOC_G_FMT,&cf);
            printf("     CAP %ux%u fourcc=%.4s\n",cf.fmt.pix_mp.width,
                   cf.fmt.pix_mp.height,(char*)&cf.fmt.pix_mp.pixelformat);
            goto done;
        }
        pos=end;
    }
    printf("  ✗ 前 4 个 NALU 后仍无 SOURCE_CHANGE\n");
done:
    close(fd); free(data); return 0;
}
