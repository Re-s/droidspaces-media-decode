/* Private AArch64 experiment: explicit KGSL capability for Firefox RDD. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include "kgsl-filter.h"

#if !defined(__aarch64__)
#error This ABI adapter is only for native GNU/Linux AArch64.
#endif

static bool enabled;
static pid_t owner;
static int kgsl_fd=-1, render_fd=-1;
static struct stat kgsl_stat, render_stat;
static _Atomic bool filter_installed;
static int (*next_open)(const char*,int,...);
static int (*next_open64)(const char*,int,...);
static int (*next_openat)(int,const char*,int,...);
static int (*next_openat64)(int,const char*,int,...);
static int (*next_ioctl)(int,unsigned long,...);
static int (*next_prctl)(int,...);
static long (*next_syscall)(long,...);
extern int dmd_firefox_system_heap_dup(void);

__attribute__((constructor(101)))
static void resolve_calls(void)
{
    next_open=dlsym(RTLD_NEXT,"open");
    next_open64=dlsym(RTLD_NEXT,"open64");
    next_openat=dlsym(RTLD_NEXT,"openat");
    next_openat64=dlsym(RTLD_NEXT,"openat64");
    next_ioctl=dlsym(RTLD_NEXT,"ioctl");
    next_prctl=dlsym(RTLD_NEXT,"prctl");
    next_syscall=dlsym(RTLD_NEXT,"syscall");
}

/* Fixed AAPCS64 argument slots preserve all six Linux syscall arguments even
 * when the caller uses libc's variadic declaration with fewer arguments. */
static long raw_call(long nr,long a,long b,long c,long d,long e,long f)
{
    register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b;
    register long x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d;
    register long x4 __asm__("x4")=e;
    register long x5 __asm__("x5")=f;
    register long x8 __asm__("x8")=nr;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8) : "memory","cc");
    if ((unsigned long)x0 >= (unsigned long)-4095) {errno=(int)-x0;return -1;}
    return x0;
}

static bool same_device(int fd,const struct stat *expected)
{
    struct stat st;
    return fstat(fd,&st)==0 && S_ISCHR(st.st_mode) &&
        st.st_dev==expected->st_dev && st.st_ino==expected->st_ino && st.st_rdev==expected->st_rdev;
}
static bool active(void) {return enabled && owner==getpid();}
static int cache_open(const char *p,struct stat *st)
{
    int fd=(int)raw_call(SYS_openat,AT_FDCWD,(long)p,O_RDWR|O_CLOEXEC,0,0,0);
    if(fd<0)return -1;
    if(fstat(fd,st)<0 || !S_ISCHR(st->st_mode)) {raw_call(SYS_close,fd,0,0,0,0,0);return -1;}
    if(fd<3) {
        int safe=fcntl(fd,F_DUPFD_CLOEXEC,3);
        raw_call(SYS_close,fd,0,0,0,0,0);
        fd=safe;
    }
    return fd;
}

__attribute__((constructor))
static void start(int argc,char **argv,char **envp)
{
    (void)envp;
    const char *setting=getenv("DMD_FIREFOX_KGSL");
    if(argc<3 || !argv || strcmp(argv[1],"-contentproc") ||
       strcmp(argv[argc-1],"rdd") || !setting || strcmp(setting,"1"))return;
    kgsl_fd=cache_open("/dev/kgsl-3d0",&kgsl_stat);
    render_fd=cache_open("/dev/dri/renderD128",&render_stat);
    if(kgsl_fd<0 || render_fd<0) {
        if(kgsl_fd>=0)raw_call(SYS_close,kgsl_fd,0,0,0,0,0);
        if(render_fd>=0)raw_call(SYS_close,render_fd,0,0,0,0,0);
        kgsl_fd=render_fd=-1;
        return;
    }
    owner=getpid();
    enabled=true;
    fprintf(stderr,"[dmd-kgsl] RDD pid=%d preopened kgsl=%d render=%d\n",owner,kgsl_fd,render_fd);
}

static int open_device(const char *path,int flags)
{
    if(!active()||!path)return -2;
    int permitted=O_ACCMODE|O_CLOEXEC|O_LARGEFILE;
    if(flags&~permitted)return -2;
    int fd=-2;
    if(!strcmp(path,"/dev/kgsl-3d0")) {
        if((flags&O_ACCMODE)!=O_RDWR)return -2;
        if(!same_device(kgsl_fd,&kgsl_stat)){errno=ESTALE;return -1;}
        fd=fcntl(kgsl_fd,F_DUPFD_CLOEXEC,3);
    } else if(!strcmp(path,"/dev/dri/renderD128")) {
        if((flags&O_ACCMODE)!=O_RDWR)return -2;
        if(!same_device(render_fd,&render_stat)){errno=ESTALE;return -1;}
        fd=fcntl(render_fd,F_DUPFD_CLOEXEC,3);
    } else if(!strcmp(path,"/dev/dma_heap/system")) {
        if((flags&O_ACCMODE)!=O_RDONLY)return -2;
        fd=dmd_firefox_system_heap_dup();
        /* The heap constructor may run after this bridge's constructor. */
        if(fd<0)return -2;
    }
    return fd;
}

__attribute__((visibility("default")))
int bridge_open(const char *path,int flags,mode_t mode) __asm__("open");
int bridge_open(const char *path,int flags,mode_t mode)
{
    int fd=open_device(path,flags);
    if(fd!=-2)return fd;
    return next_open?next_open(path,flags,mode):(int)raw_call(SYS_openat,AT_FDCWD,(long)path,flags,mode,0,0);
}
__attribute__((visibility("default")))
int bridge_open64(const char*,int,mode_t) __asm__("open64");
int bridge_open64(const char *path,int flags,mode_t mode)
{
    int fd=open_device(path,flags);
    if(fd!=-2)return fd;
    return next_open64?next_open64(path,flags,mode):(int)raw_call(SYS_openat,AT_FDCWD,(long)path,flags,mode,0,0);
}

__attribute__((visibility("default")))
int bridge_openat(int dir,const char *path,int flags,mode_t mode) __asm__("openat");
int bridge_openat(int dir,const char *path,int flags,mode_t mode)
{
    int fd=(path && path[0]=='/')?open_device(path,flags):-2;
    if(fd!=-2)return fd;
    return next_openat?next_openat(dir,path,flags,mode):(int)raw_call(SYS_openat,dir,(long)path,flags,mode,0,0);
}
__attribute__((visibility("default")))
int bridge_openat64(int,const char*,int,mode_t) __asm__("openat64");
int bridge_openat64(int dir,const char *path,int flags,mode_t mode)
{
    int fd=(path && path[0]=='/')?open_device(path,flags):-2;
    if(fd!=-2)return fd;
    return next_openat64?next_openat64(dir,path,flags,mode):(int)raw_call(SYS_openat,dir,(long)path,flags,mode,0,0);
}

struct request {unsigned long value;const char *name;};
static const struct request requests[]={
    {0xc0180902UL,"GETPROPERTY"},{0x400c0907UL,"WAITTIMESTAMP_CTXTID"},
    {0x40040914UL,"DRAWCTXT_DESTROY"},{0xc0080913UL,"DRAWCTXT_CREATE"},
    {0xc0080935UL,"GPUMEM_FREE_ID"},{0xc0300947UL,"GPUOBJ_INFO"},
    {0xc0200948UL,"GPUOBJ_IMPORT"},{0xc0300934UL,"GPUMEM_ALLOC_ID"},
    {0xc0200933UL,"TIMESTAMP_EVENT"},{0xc040094aUL,"GPU_COMMAND"}
};

__attribute__((visibility("default")))
int bridge_ioctl(int fd,unsigned long request,void *arg) __asm__("ioctl");
int bridge_ioctl(int fd,unsigned long request,void *arg)
{
    if(active()) {
        bool kgsl=same_device(fd,&kgsl_stat);
        bool drm=request==0xc0180902UL && same_device(fd,&render_stat);
        for(unsigned i=0;(kgsl||drm)&&i<sizeof(requests)/sizeof(requests[0]);i++) {
            if(request==requests[i].value) {
                int cached=kgsl?kgsl_fd:render_fd;
                if(!same_device(cached,kgsl?&kgsl_stat:&render_stat)){errno=ESTALE;return -1;}
                return (int)raw_call(SYS_ioctl,cached,request,(long)arg,0,0,0);
            }
        }
    }
    return next_ioctl?next_ioctl(fd,request,arg):(int)raw_call(SYS_ioctl,fd,request,(long)arg,0,0,0);
}

static long filtered_call(long nr,long a,long b,long c,long d,long e,long f,struct sock_fprog *program)
{
    if(!active() || atomic_load(&filter_installed) ||
       nr!=SYS_seccomp || a!=SECCOMP_SET_MODE_FILTER ||
       (b!=SECCOMP_FILTER_FLAG_TSYNC && b!=(SECCOMP_FILTER_FLAG_TSYNC|SECCOMP_FILTER_FLAG_SPEC_ALLOW)))
        return next_syscall?next_syscall(nr,a,b,c,d,e,f):raw_call(nr,a,b,c,d,e,f);
    if(!same_device(kgsl_fd,&kgsl_stat) || !same_device(render_fd,&render_stat)) {errno=ESTALE;return -1;}
    struct dmd_kgsl_filter *extended=NULL;
    uint32_t allowed[sizeof(requests)/sizeof(requests[0])];
    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)allowed[i]=(uint32_t)requests[i].value;
    /* kgsl_filter API is supplied by the separately tested generator. */
    if(dmd_kgsl_filter_create(program,kgsl_fd,render_fd,allowed,sizeof(allowed)/sizeof(allowed[0]),&extended)<0)return -1;
    c=(long)&extended->program;
    fprintf(stderr,"[dmd-kgsl] extending RDD filter %u -> %u instructions; flags=%ld\n",program->len,extended->program.len,nr==SYS_seccomp?b:0);
    long result=raw_call(nr,a,b,c,d,e,f);
    int error=errno;
    if(result==0)atomic_store(&filter_installed,true);
    dmd_kgsl_filter_free(extended);
    errno=error;
    return result;
}

__attribute__((visibility("default")))
long bridge_syscall(long nr,long a,long b,long c,long d,long e,long f) __asm__("syscall");
long bridge_syscall(long nr,long a,long b,long c,long d,long e,long f)
{
    if(nr==SYS_seccomp && a==SECCOMP_SET_MODE_FILTER && c)
        return filtered_call(nr,a,b,c,d,e,f,(struct sock_fprog*)c);
    return next_syscall?next_syscall(nr,a,b,c,d,e,f):raw_call(nr,a,b,c,d,e,f);
}

__attribute__((visibility("default")))
int bridge_prctl(int op,unsigned long a,unsigned long b,unsigned long c,unsigned long d) __asm__("prctl");
int bridge_prctl(int op,unsigned long a,unsigned long b,unsigned long c,unsigned long d)
{
    /* Firefox's non-TSYNC fallback can execute in signal handlers. Never
     * allocate, log or alter its policy; this adapter requires native TSYNC. */
    return next_prctl?next_prctl(op,a,b,c,d):(int)raw_call(SYS_prctl,op,a,b,c,d,0);
}
