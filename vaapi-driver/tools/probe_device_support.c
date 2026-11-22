/*
 * Probe_device_support - Determine whether a device can be decoded using this driver
 *
 * Usage: cc-O2-o probe_device_support probe_device_support.c
 *       ./probe_device_support /path/to/stream.h264
 *
 * It takes the key steps to drive the open path alone and reports the results at each step:
 *   S_FMT(OUTPUT)  ⁇  REQBUFS (Auto-Triven USERPTR/DMABUF/MMAPP)  ⁇  STREAMON
 *   ⁇  QBUF Several units  ⁇  et al. V4L2_EVENT_SOURCE_CHANGE
 *
 * Decision:** The receipt of SOURCE_CHANGE indicates that the solid is actually decomposition flow** and that the device is available.
 * The lack of access means the session state machine is missing.
 * Changing buffer types, heaps or permissions did not help in test runs 81–85.
 *
 * Actual results (round 85):
 *   Metric flat tablet 5/ nabu, kernel 4.14.336
 *     REQBUFS only accepts USERPTR (DMABUF and MMAP EINVAL)
 *     Every step ahead is a success, but SOURCE never gets here.
 *     IRQ 510 etc. Time-to-time comparison: Free 50/10s vs feed 49/12s - Solid Zero Response
 *     ~ Not available
 *   The other one has dma_heap.
 *     REQBUFS Accept DMABUF, two-part normal consultation, H264 300/300 frame
 *     Pixels match the bytes of soft resolution * Available
 *
 * ⁇  Do not use vainfo to judge whether this driver is available -- it will hang on this platform, and then it will be used to determine whether the driver is available.
 * Even if you specify a driver that does not exist.
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

    /* 4 block lines for each event; msm_vidc may require multiple units to occur */
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
        printf("  QBUF[%d] %zu bytes = %s\n",i,n,q==0?"ok":strerror(errno));

        struct pollfd pf={.fd=fd,.events=POLLPRI};
        poll(&pf,1,2000);
        if(pf.revents&POLLPRI){
            struct v4l2_event ev; memset(&ev,0,sizeof ev);
            ioctl(fd,VIDIOC_DQEVENT,&ev);
            printf("  ✓ Events type=%u At %d Unit is arriving.\n",ev.type,i);
            struct v4l2_format cf; memset(&cf,0,sizeof cf);
            cf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl(fd,VIDIOC_G_FMT,&cf);
            printf("    CAP Resolution %ux%u\n",cf.fmt.pix_mp.width,cf.fmt.pix_mp.height);
            goto done;
        }
        printf("    poll timed out(revents=0x%x)\n",pf.revents);
    }
    printf("  ✗ Yes. 4 No units available SOURCE_CHANGE\n");
done:
    fclose(fp); close(fd); return 0;
}
