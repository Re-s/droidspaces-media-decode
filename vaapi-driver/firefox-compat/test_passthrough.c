/* SPDX-License-Identifier: MIT
 * Non-RDD libc ABI/cancellation smoke test for the native AArch64 preload.
 * Host: compile only. In the container as Gold:
 *   cc -std=c11 -O2 -Wall -Wextra -Werror test_passthrough.c -ldl -pthread -o test_passthrough
 *   LD_PRELOAD=/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so \
 *     DMD_FIREFOX_KGSL=1 ./test_passthrough
 *   LD_PRELOAD=/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so \
 *     DMD_FIREFOX_KGSL=1 ./test_passthrough -contentproc 0 tab
 * This test never installs a seccomp filter or calls a GPU ioctl.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures;
static const char *const variants[] = {"open", "open64", "openat", "openat64"};

static void check(int passed, const char *name, int error)
{
    fprintf(stderr, "%s: %s%s%s\n", passed ? "PASS" : "FAIL", name,
            error ? ": " : "", error ? strerror(error) : "");
    if (!passed)
        ++failures;
}

static void check_device_fds(const char *stage)
{
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) {
        check(0, "/proc/self/fd inventory", errno);
        return;
    }
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end;
        (void)strtol(entry->d_name, &end, 10);
        if (!entry->d_name[0] || *end)
            continue;
        char path[PATH_MAX], target[PATH_MAX];
        int count = snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
        if (count < 0 || (size_t)count >= sizeof(path))
            continue;
        ssize_t size = readlink(path, target, sizeof(target) - 1);
        if (size < 0)
            continue;
        target[size] = 0;
        if (!strncmp(target, "/dev/kgsl", 9) ||
            !strncmp(target, "/dev/dma_heap/", 14) ||
            !strncmp(target, "/dev/dri/renderD", 16)) {
            fprintf(stderr, "Unexpected non-RDD device fd at %s: %s -> %s\n",
                    stage, entry->d_name, target);
            ++found;
        }
    }
    closedir(directory);
    check(!found, stage, 0);
}

static int create_file(unsigned variant, int directory, const char *absolute,
                       const char *relative, mode_t mode)
{
    const int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
    switch (variant) {
    case 0: return open(absolute, flags, mode);
    case 1: return open64(absolute, flags, mode);
    case 2: return openat(directory, relative, flags, mode);
    default: return openat64(directory, relative, flags, mode);
    }
}

static void test_files(const char *root, int directory)
{
    const mode_t modes[] = {0640, 0604, 0620, 0644};
    const char payload[] = "DroidSpaces libc passthrough\n";
    for (unsigned i = 0; i < 4; ++i) {
        char relative[32], absolute[PATH_MAX], label[96];
        snprintf(relative, sizeof(relative), "created-%u", i);
        snprintf(absolute, sizeof(absolute), "%s/%s", root, relative);
        errno = 0;
        int fd = create_file(i, directory, absolute, relative, modes[i]);
        snprintf(label, sizeof(label), "%s O_CREAT descriptor", variants[i]);
        check(fd >= 0, label, fd < 0 ? errno : 0);
        if (fd < 0)
            continue;
        struct stat st;
        int result = fstat(fd, &st);
        snprintf(label, sizeof(label), "%s O_CREAT permission argument", variants[i]);
        check(result == 0 && (st.st_mode & 0777) == modes[i], label,
              result < 0 ? errno : 0);
        int flags = fcntl(fd, F_GETFD);
        check(flags >= 0 && (flags & FD_CLOEXEC), "open preserves CLOEXEC", 0);
        ssize_t written = write(fd, payload, sizeof(payload));
        off_t offset = lseek(fd, 0, SEEK_SET);
        char readback[sizeof(payload)] = {0};
        ssize_t got = read(fd, readback, sizeof(readback));
        check(written == (ssize_t)sizeof(payload) && offset == 0 &&
              got == (ssize_t)sizeof(payload) && !memcmp(readback, payload, sizeof(payload)),
              "created file read/write/seek", 0);
        check(close(fd) == 0, "created file close", 0);
    }
    typedef int (*fortified_open)(const char *, int);
    fortified_open open_two = (fortified_open)dlsym(RTLD_DEFAULT, "__open_2");
    if (open_two) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/created-0", root);
        errno = 0;
        int fd = open_two(path, O_RDONLY | O_CLOEXEC);
        check(fd >= 0, "fortified __open_2 existing-file read", fd < 0 ? errno : 0);
        if (fd >= 0) {
            char bytes[sizeof(payload)] = {0};
            ssize_t got = read(fd, bytes, sizeof(bytes));
            check(got == (ssize_t)sizeof(payload) && !memcmp(bytes, payload, sizeof(payload)),
                  "fortified __open_2 contents", 0);
            close(fd);
        }
    } else {
        fprintf(stderr, "SKIP: __open_2 symbol is not exported by this libc\n");
    }
}

static void test_ioctl(void)
{
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) < 0) {
        check(0, "pipe for FIONREAD", errno);
        return;
    }
    const char payload[] = "pipe-data";
    ssize_t written = write(descriptors[1], payload, sizeof(payload));
    int available = -1;
    errno = 0;
    int result = ioctl(descriptors[0], FIONREAD, &available);
    check(written == (ssize_t)sizeof(payload) && result == 0 &&
          available == (int)sizeof(payload), "non-RDD ioctl FIONREAD pipe", result < 0 ? errno : 0);
    close(descriptors[0]);
    close(descriptors[1]);
}

static void test_syscall_and_prctl(int directory)
{
    long page = sysconf(_SC_PAGESIZE);
    int fd = openat(directory, "mmap-file", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (page <= 0 || fd < 0) {
        check(0, "mmap test setup", errno);
        if (fd >= 0)
            close(fd);
        return;
    }
    const char marker[] = "page-two-sixth-syscall-argument";
    int sized = ftruncate(fd, (off_t)page * 2);
    ssize_t stored = pwrite(fd, marker, sizeof(marker), (off_t)page);
    check(sized == 0 && stored == (ssize_t)sizeof(marker), "file-backed mmap setup", 0);
    errno = 0;
    long address = syscall(SYS_mmap, 0UL, (unsigned long)page,
                           (long)(PROT_READ | PROT_WRITE), (long)MAP_PRIVATE,
                           (long)fd, page);
    check(address != -1, "syscall mmap six arguments", address == -1 ? errno : 0);
    if (address != -1) {
        void *mapping = (void *)(uintptr_t)address;
        check(!memcmp(mapping, marker, sizeof(marker)), "mmap nonzero sixth-argument file offset", 0);
        ((char *)mapping)[0] = 'P';
        check(munmap(mapping, (size_t)page) == 0, "mmap protection/munmap", 0);
    }
    errno = 0;
    address = syscall(SYS_mmap, 0UL, 0UL, (long)PROT_READ, (long)MAP_PRIVATE,
                      (long)fd, 0L);
    check(address == -1 && errno == EINVAL, "syscall errno EINVAL passthrough", errno);
    errno = 0;
    long result = syscall(SYS_close, -1L, 0L, 0L, 0L, 0L, 0L);
    check(result == -1 && errno == EBADF, "syscall errno EBADF passthrough", errno);
    close(fd);

    char original[16] = {0}, actual[16] = {0};
    int got = prctl(PR_GET_NAME, (unsigned long)original, 0UL, 0UL, 0UL);
    check(got == 0, "prctl original GET_NAME", got < 0 ? errno : 0);
    const char expected[] = "dmd-passthrough";
    int set = prctl(PR_SET_NAME, (unsigned long)expected, 0UL, 0UL, 0UL);
    errno = 0;
    got = prctl(PR_GET_NAME, (unsigned long)actual, 0UL, 0UL, 0UL);
    check(set == 0 && got == 0 && !strcmp(actual, expected),
          "prctl GET_NAME preserves argument pointer", got < 0 ? errno : 0);
    prctl(PR_SET_NAME, (unsigned long)original, 0UL, 0UL, 0UL);
}

struct fifo_worker {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int started, directory;
    unsigned variant;
    char path[PATH_MAX];
};

static void *open_fifo(void *argument)
{
    struct fifo_worker *worker = argument;
    pthread_mutex_lock(&worker->mutex);
    worker->started = 1;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->mutex);
    int fd;
    switch (worker->variant) {
    case 0: fd = open(worker->path, O_RDONLY | O_CLOEXEC); break;
    case 1: fd = open64(worker->path, O_RDONLY | O_CLOEXEC); break;
    case 2: fd = openat(worker->directory, "fifo", O_RDONLY | O_CLOEXEC); break;
    default: fd = openat64(worker->directory, "fifo", O_RDONLY | O_CLOEXEC); break;
    }
    if (fd >= 0)
        close(fd);
    /* Returning normally, even with EINTR, does not preserve cancellation. */
    return (void *)(uintptr_t)1;
}

static void test_cancellation(const char *root, int directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/fifo", root);
    if (mkfifo(path, 0600) < 0) {
        check(0, "FIFO cancellation setup", errno);
        return;
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        struct fifo_worker worker = {.mutex = PTHREAD_MUTEX_INITIALIZER,
            .ready = PTHREAD_COND_INITIALIZER, .directory = directory,
            .variant = variant};
        snprintf(worker.path, sizeof(worker.path), "%s", path);
        pthread_t thread;
        int result = pthread_create(&thread, NULL, open_fifo, &worker);
        check(result == 0, "create FIFO worker", result);
        if (result)
            continue;
        struct timespec limit;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_sec += 3;
        pthread_mutex_lock(&worker.mutex);
        while (!worker.started) {
            result = pthread_cond_timedwait(&worker.ready, &worker.mutex, &limit);
            if (result)
                break;
        }
        pthread_mutex_unlock(&worker.mutex);
        struct timespec settle = {.tv_nsec = 100000000};
        nanosleep(&settle, NULL);
        int canceled = pthread_cancel(thread);
        void *returned = NULL;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_sec += 3;
        result = pthread_timedjoin_np(thread, &returned, &limit);
        char label[128];
        snprintf(label, sizeof(label), "%s FIFO remains a pthread cancellation point", variants[variant]);
        check(canceled == 0 && result == 0 && returned == PTHREAD_CANCELED,
              label, result ? result : canceled);
        if (result) {
            /* A raw-syscall interposer can lose libc cancellation. Unblock its
             * FIFO to reclaim the thread, but keep the timed-join test failed. */
            int writer = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (writer >= 0)
                close(writer);
            pthread_join(thread, &returned);
        }
        pthread_cond_destroy(&worker.ready);
        pthread_mutex_destroy(&worker.mutex);
    }
    unlink(path);
}

int main(int argc, char **argv)
{
#if !defined(__aarch64__) || !defined(__GLIBC__)
    const int supported = 0;
#else
    const int supported = 1;
#endif
    if (!supported) {
        fprintf(stderr, "SKIP: native GNU/Linux AArch64 test; compile only on this host\n");
        return 77;
    }
    int content = 0, rdd = 0;
    for (int i = 1; i < argc; ++i) {
        content |= !strcmp(argv[i], "-contentproc");
        rdd |= !strcmp(argv[i], "rdd");
    }
    if (content && rdd) {
        fprintf(stderr, "This tests parent/content passthrough; do not pass RDD argv\n");
        return 2;
    }
    const char *enabled = getenv("DMD_FIREFOX_KGSL");
    if (!enabled || strcmp(enabled, "1") || !getenv("LD_PRELOAD") ||
        !dlsym(RTLD_DEFAULT, "dmd_firefox_system_heap_dup")) {
        fprintf(stderr, "Run with DMD_FIREFOX_KGSL=1 and LD_PRELOAD=.../libdmd-firefox-compat.so\n");
        return 2;
    }
    Dl_info owner;
    void *open_symbol = dlsym(RTLD_DEFAULT, "open");
    int loaded = open_symbol && dladdr(open_symbol, &owner) &&
                 owner.dli_fname && strstr(owner.dli_fname, "libdmd-firefox-compat");
    check(loaded, "open resolves to the tested compatibility library", 0);
    fprintf(stderr, "Non-RDD smoke: %s argv, pid=%ld\n", content ? "content" : "parent", (long)getpid());
    alarm(30);
    check_device_fds("non-RDD startup has no KGSL/render/heap preopens");
    char root[] = "/tmp/dmd-firefox-passthrough.XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        perror("open scratch directory");
        rmdir(root);
        return 1;
    }
    mode_t previous_mask = umask(0);
    test_files(root, directory);
    umask(previous_mask);
    test_ioctl();
    test_syscall_and_prctl(directory);
    test_cancellation(root, directory);
    for (unsigned i = 0; i < 4; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "created-%u", i);
        unlinkat(directory, path, 0);
    }
    unlinkat(directory, "mmap-file", 0);
    close(directory);
    rmdir(root);
    check_device_fds("non-RDD finish has no KGSL/render/heap preopens");
    fprintf(stderr, "Passthrough smoke: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
