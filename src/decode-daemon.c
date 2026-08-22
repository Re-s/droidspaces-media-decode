#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#define MAX_FRAME (8*1024*1024)
#define TIMEOUT 5000000
static volatile int running=1;
static void sig(int s){(void)s;running=0;}
static int recv_all(int fd,void*b,size_t l){char*p=b;while(l>0){ssize_t n=read(fd,p,l);if(n<=0)return -1;p+=n;l-=n;}return 0;}
static int send_all(int fd,const void*b,size_t l){const char*p=b;while(l>0){ssize_t n=write(fd,p,l);if(n<=0)return -1;p+=n;l-=n;}return 0;}
int main(int argc,char**argv){
    int port=argc>1?atoi(argv[1]):20003;
    signal(SIGINT,sig);signal(SIGTERM,sig);signal(SIGPIPE,SIG_IGN);
    int srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in a={AF_INET,htons(port),{htonl(INADDR_LOOPBACK)}};
    bind(srv,(struct sockaddr*)&a,sizeof(a));listen(srv,1);
    int flag=1;setsockopt(srv,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof(flag));
    fprintf(stderr,"listening on %d\n",port);fflush(stderr);
    while(running){
        int cli=accept(srv,NULL,NULL);if(cli<0)continue;
        setsockopt(cli,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt));
        fprintf(stderr,"client connected\n");fflush(stderr);
        AMediaFormat*f=AMediaFormat_new();
        AMediaFormat_setString(f,AMEDIAFORMAT_KEY_MIME,"video/avc");
        AMediaFormat_setInt32(f,AMEDIAFORMAT_KEY_WIDTH,1920);
        AMediaFormat_setInt32(f,AMEDIAFORMAT_KEY_HEIGHT,1080);
        AMediaFormat_setInt32(f,AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,MAX_FRAME);
        AMediaCodec*c=AMediaCodec_createDecoderByType("video/avc");
        if(!c){fprintf(stderr,"no codec\n");close(cli);continue;}
        media_status_t s=AMediaCodec_configure(c,f,NULL,NULL,0);
        fprintf(stderr,"configure: %d\n",s);fflush(stderr);
        if(s!=0){AMediaCodec_delete(c);AMediaFormat_delete(f);close(cli);continue;}
        s=AMediaCodec_start(c);
        fprintf(stderr,"start: %d\n",s);fflush(stderr);
        if(s!=0){AMediaCodec_stop(c);AMediaCodec_delete(c);AMediaFormat_delete(f);close(cli);continue;}
        uint8_t*buf=malloc(MAX_FRAME);uint8_t*csd=malloc(MAX_FRAME);size_t csd_len=0;
        int w=1920,h=1080,nalu_count=0,input_ok=0,output_ok=0;
        fprintf(stderr,"loop start\n");fflush(stderr);
        while(running){
            uint32_t sz;if(recv_all(cli,&sz,4)<0){fprintf(stderr,"recv hdr fail\n");break;}
            sz=ntohl(sz);if(sz==0||sz>MAX_FRAME){fprintf(stderr,"bad size %u\n",sz);break;}
            if(recv_all(cli,buf,sz)<0){fprintf(stderr,"recv data fail\n");break;}
            nalu_count++;
            int t=(sz>3)?(buf[3]&0x1f):0;
            if(t==7||t==8){if(csd_len+sz<MAX_FRAME){memcpy(csd+csd_len,buf,sz);csd_len+=sz;}continue;}
            if(csd_len>0){
                ssize_t ci=AMediaCodec_dequeueInputBuffer(c,TIMEOUT);
                if(ci>=0){size_t bs;uint8_t*b=AMediaCodec_getInputBuffer(c,ci,&bs);if(b&&bs>=csd_len){memcpy(b,csd,csd_len);AMediaCodec_queueInputBuffer(c,ci,0,csd_len,0,0x2);fprintf(stderr,"CSD sent\n");}else{fprintf(stderr,"CSD buffer too small\n");}}
                else{fprintf(stderr,"CSD dequeueInputBuffer failed: %zd\n",ci);}
                csd_len=0;
            }
            ssize_t bi=AMediaCodec_dequeueInputBuffer(c,TIMEOUT);
            if(bi>=0){size_t bs;uint8_t*b=AMediaCodec_getInputBuffer(c,bi,&bs);if(b&&bs>=sz){memcpy(b,buf,sz);AMediaCodec_queueInputBuffer(c,bi,0,sz,0,0);input_ok++;}else{fprintf(stderr,"frame buf too small\n");}}
            else{fprintf(stderr,"dequeueInputBuffer fail\n");}
            AMediaCodecBufferInfo info;ssize_t oi=AMediaCodec_dequeueOutputBuffer(c,&info,1000);
            if(oi>=0){
                size_t os;uint8_t*ob=AMediaCodec_getOutputBuffer(c,oi,&os);
                fprintf(stderr,"OUTPUT frame %dx%d %d\n",w,h,info.size);fflush(stderr);
                uint32_t hdr[3]={htonl(w),htonl(h),htonl(info.size)};
                if(info.size>0&&ob){send_all(cli,hdr,12);size_t off=info.offset,rem=info.size;while(rem>0){size_t ch=(rem>262144)?262144:rem;if(send_all(cli,ob+off,ch)<0)break;off+=ch;rem-=ch;}}
                AMediaCodec_releaseOutputBuffer(c,oi,0);output_ok++;
            }else if(oi==AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED){
                AMediaFormat*of=AMediaCodec_getOutputFormat(c);
                AMediaFormat_getInt32(of,AMEDIAFORMAT_KEY_WIDTH,&w);
                AMediaFormat_getInt32(of,AMEDIAFORMAT_KEY_HEIGHT,&h);
                fprintf(stderr,"FORMAT %dx%d\n",w,h);fflush(stderr);
                AMediaFormat_delete(of);
            }else if(oi==AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED){}
            else{fprintf(stderr,"dequeueOutput: %zd\n",oi);}
        }
        fprintf(stderr,"loop end: %d nalu, %d in, %d out\n",nalu_count,input_ok,output_ok);fflush(stderr);
        AMediaCodec_signalEndOfInputStream(c);
        for(int r=0;r<30;r++){AMediaCodecBufferInfo info;ssize_t oi=AMediaCodec_dequeueOutputBuffer(c,&info,500000);if(oi>=0){size_t os;uint8_t*ob=AMediaCodec_getOutputBuffer(c,oi,&os);fprintf(stderr,"FLUSH %dx%d %d\n",w,h,info.size);fflush(stderr);uint32_t hdr[3]={htonl(w),htonl(h),htonl(info.size)};if(info.size>0&&ob){send_all(cli,hdr,12);size_t off=info.offset,rem=info.size;while(rem>0){size_t ch=(rem>262144)?262144:rem;if(send_all(cli,ob+off,ch)<0)break;off+=ch;rem-=ch;}}AMediaCodec_releaseOutputBuffer(c,oi,0);output_ok++;}else break;}
        fprintf(stderr,"total: %d out\n",output_ok);fflush(stderr);
        AMediaCodec_stop(c);AMediaCodec_delete(c);AMediaFormat_delete(f);free(buf);free(csd);close(cli);
    }
    close(srv);return 0;
}
