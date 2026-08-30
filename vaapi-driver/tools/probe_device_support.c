/*
 * probe_device_support —— 判定一台设备能否用本驱动解码
 *
 * 用法：cc -O2 -o probe_device_support probe_device_support.c
 *       ./probe_device_support /path/to/stream.h264
 *
 * 它把驱动 open 路径的关键几步单独走一遍，并在每步报告结果：
 *   S_FMT(OUTPUT) → REQBUFS(自动试 USERPTR/DMABUF/MMAP) → STREAMON
 *   → QBUF 若干单元 → 等 V4L2_EVENT_SOURCE_CHANGE
 *
 * 判据：**能收到 SOURCE_CHANGE 就说明固件真的在解析码流**，这台设备可用。
 * 收不到就是会话状态机没到 START_DONE，驱动帮不上 —— 那是固件/内核层面
 * 的事，换缓冲类型、换 heap、加权限都没用（第 81~85 轮逐一试过）。
 *
 * 实测结果（第 85 轮）：
 *   小米平板 5 / nabu，内核 4.14.336
 *     REQBUFS 只接受 USERPTR（DMABUF 与 MMAP 均 EINVAL）
 *     前面每一步都成功，但 SOURCE_CHANGE 永不到达
 *     IRQ 510 等时长对照：空闲 50/10s vs 喂料 49/12s —— 固件零响应
 *     → 不可用
 *   另一台有 dma_heap 的设备
 *     REQBUFS 接受 DMABUF，两段式协商正常，H264 300/300 帧
 *     像素与软解逐字节一致 → 可用
 *
 * ⚠️ 别用 vainfo 判断本驱动是否可用 —— 它在本平台会挂住，
 * 即使指定不存在的驱动名也一样。
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
int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"/tmp/loc.h264";
    int fd=open("/dev/video32",O_RDWR|O_CLOEXEC);
    struct v4l2_format f; memset(&f,0,sizeof f);
    f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width=1280; f.fmt.pix_mp.height=720;
    f.fmt.pix_mp.pixelformat=v4l2_fourcc('H','2','6','4');
    f.fmt.pix_mp.num_planes=1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage=4194304;
    ioctl(fd,VIDIOC_S_FMT,&f);
    size_t in_size=f.fmt.pix_mp.plane_fmt[0].sizeimage;
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count=4; rb.type=f.type; rb.memory=V4L2_MEMORY_USERPTR;
    ioctl(fd,VIDIOC_REQBUFS,&rb);
    struct v4l2_event_subscription sub; memset(&sub,0,sizeof sub);
    sub.type=V4L2_EVENT_SOURCE_CHANGE; ioctl(fd,VIDIOC_SUBSCRIBE_EVENT,&sub);
    unsigned t=f.type; ioctl(fd,VIDIOC_STREAMON,&t);

    FILE*fp=fopen(path,"rb");
    void*bufs[4];
    for(int i=0;i<4;i++) bufs[i]=aligned_alloc(4096,in_size);

    /* 连喂 4 个块，每次等事件；msm_vidc 可能需要多个单元才出事件 */
    for(int i=0;i<4;i++){
        size_t n=fread(bufs[i],1,128*1024,fp);
        if(!n) break;
        struct v4l2_buffer b; struct v4l2_plane p[1];
        memset(&b,0,sizeof b); memset(p,0,sizeof p);
        b.type=f.type; b.memory=V4L2_MEMORY_USERPTR; b.index=i;
        b.m.planes=p; b.length=1;
        p[0].m.userptr=(unsigned long)bufs[i];
        p[0].length=in_size; p[0].bytesused=n;
        int q=ioctl(fd,VIDIOC_QBUF,&b);
        printf("  QBUF[%d] %zu 字节 = %s\n",i,n,q==0?"ok":strerror(errno));

        struct pollfd pf={.fd=fd,.events=POLLPRI};
        poll(&pf,1,2000);
        if(pf.revents&POLLPRI){
            struct v4l2_event ev; memset(&ev,0,sizeof ev);
            ioctl(fd,VIDIOC_DQEVENT,&ev);
            printf("  ✓ 事件 type=%u 在第 %d 个单元后到达\n",ev.type,i);
            struct v4l2_format cf; memset(&cf,0,sizeof cf);
            cf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl(fd,VIDIOC_G_FMT,&cf);
            printf("    CAP 分辨率 %ux%u\n",cf.fmt.pix_mp.width,cf.fmt.pix_mp.height);
            goto done;
        }
        printf("    poll 超时（revents=0x%x）\n",pf.revents);
    }
    printf("  ✗ 喂完 4 个单元仍无 SOURCE_CHANGE\n");
done:
    fclose(fp); close(fd); return 0;
}
