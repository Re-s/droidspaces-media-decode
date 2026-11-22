/* SPDX-License-Identifier: MIT
 * Firefox-only, pre-sandbox system-heap capability for the VA-API driver.
 * This library does not interpose open(), ioctl(), or sandbox entry points.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct heap_cache {
    int fd;
    int error;
    pid_t owner;
    dev_t device;
    ino_t inode;
    dev_t rdevice;
};

/* Written once by the loader, before application threads are started.  Keep
 * this descriptor for the process lifetime; callers own only their duplicates.
 * There is deliberately no destructor which could close a recycled fd or race
 * an application thread during library unload/process termination. */
static struct heap_cache cache = { .fd = -1, .error = ENODEV };

static bool is_rdd_child(int argc, char *const argv[])
{
    if (argc < 3 || !argv || !argv[1] || !argv[argc-1])
        return false;
    return strcmp(argv[1], "-contentproc") == 0 &&
           strcmp(argv[argc-1], "rdd") == 0;
}

#ifdef DMD_FIREFOX_HEAP_TEST_PATH
/* Test builds substitute a host character device; no runtime path override. */
#define SYSTEM_HEAP_PATH DMD_FIREFOX_HEAP_TEST_PATH
#else
#define SYSTEM_HEAP_PATH "/dev/dma_heap/system"
#endif

#ifndef DMD_FIREFOX_HEAP_OPEN
#define DMD_FIREFOX_HEAP_OPEN open
#endif

static void preopen_system_heap(int argc, char *const argv[])
{
    if (!is_rdd_child(argc, argv))
        return;
#ifdef DMD_FIREFOX_KGSL_BUILD
    const char *enabled = getenv("DMD_FIREFOX_KGSL");
    if (!enabled || strcmp(enabled, "1"))
        return;
#endif

    int fd = DMD_FIREFOX_HEAP_OPEN(SYSTEM_HEAP_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        cache.error = errno;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int error = errno;
        close(fd);
        cache.error = error;
        return;
    }
    if (!S_ISCHR(st.st_mode)) {
        close(fd);
        cache.error = ENODEV;
        return;
    }
    if (fd < 3) {
        int safe = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        int error = errno;
        close(fd);
        if (safe < 0) {cache.error = error;return;}
        fd = safe;
    }
    cache.fd = fd;
    cache.owner = getpid();
    cache.device = st.st_dev;
    cache.inode = st.st_ino;
    cache.rdevice = st.st_rdev;
    cache.error = 0;
}

#ifndef DMD_FIREFOX_HEAP_NO_CONSTRUCTOR
/* glibc passes the original process argc/argv to ELF constructors.  This is
 * intentionally GNU/Linux-specific and does not run again after fork(). */
__attribute__((constructor))
static void initialize(int argc, char **argv, char **envp)
{
    (void)envp;
    int saved_errno = errno;
    preopen_system_heap(argc, argv);
    errno = saved_errno;
}
#endif

/* ABI: return a new, caller-owned CLOEXEC fd, or -1 with errno.  This is only
 * an allocation capability; the driver still performs DMA_HEAP_IOCTL_ALLOC. */
__attribute__((visibility("default")))
int dmd_firefox_system_heap_dup(void)
{
    if (cache.fd < 0) {
        errno = cache.error ? cache.error : ENODEV;
        return -1;
    }
    if (cache.owner != getpid()) {
        errno = ECHILD;
        return -1;
    }

    /* Duplicate first, then inspect the stable duplicate.  If something in the
     * host closed/reused our cached numeric fd, never give that new object to
     * the driver.  No fstat()/dup() identity-check race is introduced. */
    int fd = fcntl(cache.fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return -1;
    struct stat st;
    int error = 0;
    if (fstat(fd, &st) < 0)
        error = errno;
    else if (!S_ISCHR(st.st_mode) || st.st_dev != cache.device ||
             st.st_ino != cache.inode || st.st_rdev != cache.rdevice)
        error = ESTALE;
    if (error) {
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
