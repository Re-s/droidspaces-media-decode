/* SPDX-License-Identifier: MIT
 * Native AArch64 kernel verification of the explicit KGSL policy extension.
 *
 * Build only on the host; run as Gold in the test container, never via LD_PRELOAD:
 *   cc -std=c11 -O2 -Wall -Wextra -Werror test_kernel.c kgsl-filter.c -o test_kernel
 *   ./test_kernel [--kgsl /dev/kgsl-3d0] [--render /dev/dri/renderD128]
 *
 * Only a forked child sets NO_NEW_PRIVS and installs the generated filter.
 * No GPU objects/jobs are created: the permitted ioctl probes use NULL arguments.
 */
#define _GNU_SOURCE
#include "kgsl-filter.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS UINT32_C(0x80000000)
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

/* The generated extension is specifically AArch64. Keep the original policy
 * and raw probes on that same ABI even when doing a host compilation check. */
enum {
    NR_DUP3 = 24,
    NR_IOCTL = 29,
    NR_CLOSE = 57,
    NR_GETPPID = 173,
    NR_EXECVE = 221,
    NR_EXECVEAT = 281,
    NR_IO_URING_SETUP = 425,
    NR_IO_URING_ENTER = 426,
    NR_IO_URING_REGISTER = 427,
    NR_CLOSE_RANGE = 436
};

#define DENY_SYSCALL(nr, error) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (error))

static int failures;

static void check(int condition, const char *name, long result, int error)
{
    fprintf(stderr, "%s: %s (result=%ld errno=%d:%s)\n",
            condition ? "PASS" : "FAIL", name, result, error,
            error ? strerror(error) : "none");
    if (!condition)
        ++failures;
}

static void denied(const char *name, long result, int error)
{
    check(result == -1 && errno == error, name, result, errno);
}

static int device_open(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    struct stat st;
    if (fd < 0)
        return -1;
    if (fd < 3 || fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
        int saved = errno;
        close(fd);
        errno = saved ? saved : ENODEV;
        return -1;
    }
    return fd;
}

struct kernel_probe {
    long result;
    int error;
};

static struct kernel_probe getproperty(int fd)
{
    errno = 0;
    long result = syscall(NR_IOCTL, (unsigned long)fd,
                          (unsigned long)DMD_KGSL_GETPROPERTY, (void *)NULL);
    return (struct kernel_probe){result, errno};
}

static int child_test(const char *kgsl_path, const char *render_path)
{
    /* An accidental driver stall must not leave a filtered process behind. */
    alarm(20);
    int kgsl = device_open(kgsl_path);
    if (kgsl < 0) {
        fprintf(stderr, "open KGSL %s: %s\n", kgsl_path, strerror(errno));
        return 1;
    }
    int render = device_open(render_path);
    if (render < 0) {
        fprintf(stderr, "open render %s: %s\n", render_path, strerror(errno));
        close(kgsl);
        return 1;
    }
    int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    int pipefds[2];
    if (nullfd < 0 || pipe2(pipefds, O_CLOEXEC) < 0) {
        perror("open test descriptors");
        return 1;
    }
    fprintf(stderr, "Native kernel test: pid=%ld uid=%ld kgsl=%d render=%d\n",
            (long)getpid(), (long)getuid(), kgsl, render);

    /* Establish the device's actual NULL-argument error before filtering. A
     * pre-existing outer policy returning EPERM makes this an invalid test. */
    struct kernel_probe before_kgsl = getproperty(kgsl);
    struct kernel_probe before_render = getproperty(render);
    check(before_kgsl.result == -1 && before_kgsl.error != EPERM,
          "unfiltered KGSL GETPROPERTY(NULL) reaches device",
          before_kgsl.result, before_kgsl.error);
    check(before_render.result == -1 && before_render.error != EPERM,
          "unfiltered render GETPROPERTY(NULL) reaches device",
          before_render.result, before_render.error);
    if (failures)
        return 1;

    struct sock_filter original_instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, nr)),
        DENY_SYSCALL(NR_IOCTL, EPERM),
        DENY_SYSCALL(NR_GETPPID, EACCES),
        DENY_SYSCALL(NR_EXECVE, EACCES),
        DENY_SYSCALL(NR_EXECVEAT, EACCES),
        DENY_SYSCALL(NR_IO_URING_SETUP, EACCES),
        DENY_SYSCALL(NR_IO_URING_ENTER, EACCES),
        DENY_SYSCALL(NR_IO_URING_REGISTER, EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    struct sock_fprog original = {
        .len = (unsigned short)(sizeof(original_instructions) /
                                sizeof(original_instructions[0])),
        .filter = original_instructions
    };
    const uint32_t requests[] = {DMD_KGSL_GETPROPERTY};
    struct dmd_kgsl_filter *extended = NULL;
    if (dmd_kgsl_filter_create(&original, kgsl, render, requests,
                              sizeof(requests) / sizeof(requests[0]), &extended) < 0) {
        perror("dmd_kgsl_filter_create");
        return 1;
    }
    check(extended && extended->original_length == original.len &&
          extended->program.len == extended->prefix_length + original.len &&
          memcmp(extended->program.filter + extended->prefix_length,
                 original.filter, sizeof(original_instructions)) == 0,
          "original BPF remains byte-for-byte unchanged", 0, 0);
    if (failures) {
        dmd_kgsl_filter_free(extended);
        return 1;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &extended->program) < 0) {
        perror("install child seccomp filter");
        dmd_kgsl_filter_free(extended);
        return 1;
    }
    dmd_kgsl_filter_free(extended);

    struct kernel_probe after = getproperty(kgsl);
    check(after.result == before_kgsl.result && after.error == before_kgsl.error,
          "only cached KGSL GETPROPERTY reaches same kernel result",
          after.result, after.error);
    after = getproperty(render);
    check(after.result == before_render.result && after.error == before_render.error,
          "cached render GETPROPERTY reaches same kernel result",
          after.result, after.error);

    errno = 0;
    denied("same request on /dev/null stays blocked",
           syscall(NR_IOCTL, (unsigned long)nullfd,
                   (unsigned long)DMD_KGSL_GETPROPERTY, (void *)NULL), EPERM);
    errno = 0;
    denied("same ioctl number with different size stays blocked",
           syscall(NR_IOCTL, (unsigned long)kgsl,
                   (unsigned long)(DMD_KGSL_GETPROPERTY ^ UINT32_C(0x00010000)),
                   (void *)NULL), EPERM);
    errno = 0;
    denied("different ioctl namespace stays blocked",
           syscall(NR_IOCTL, (unsigned long)render, (unsigned long)FIONREAD,
                   (void *)NULL), EPERM);
    errno = 0;
    denied("ioctl fd with nonzero upper bits stays blocked",
           syscall(NR_IOCTL, (unsigned long)kgsl | (UINT64_C(1) << 32),
                   (unsigned long)DMD_KGSL_GETPROPERTY, (void *)NULL), EPERM);
    errno = 0;
    denied("ioctl request with nonzero upper bits stays blocked",
           syscall(NR_IOCTL, (unsigned long)kgsl,
                   (unsigned long)DMD_KGSL_GETPROPERTY | (UINT64_C(1) << 32),
                   (void *)NULL), EPERM);

    const int caches[] = {kgsl, render};
    for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); ++i) {
        int fd = caches[i];
        errno = 0;
        denied("close cached descriptor blocked", syscall(NR_CLOSE, fd), EPERM);
        errno = 0;
        denied("high-word close cannot free cached slot",
               syscall(NR_CLOSE, (unsigned long)fd | (UINT64_C(1) << 32)), EPERM);
        errno = 0;
        denied("dup3 cannot overwrite cached slot",
               syscall(NR_DUP3, nullfd, fd, O_CLOEXEC), EPERM);
        errno = 0;
        denied("high-word dup3 target cannot overwrite cached slot",
               syscall(NR_DUP3, nullfd,
                       (unsigned long)fd | (UINT64_C(1) << 32), O_CLOEXEC), EPERM);
        errno = 0;
        denied("close_range cannot free cached slot",
               syscall(NR_CLOSE_RANGE, (unsigned)fd, (unsigned)fd, 0), EPERM);
        errno = 0;
        denied("high-word close_range cannot free cached slot",
               syscall(NR_CLOSE_RANGE,
                       (unsigned long)fd | (UINT64_C(1) << 32),
                       (unsigned long)fd | (UINT64_C(1) << 32), 0), EPERM);
        errno = 0;
        denied("close_range CLOEXEC cannot alter cached slot",
               syscall(NR_CLOSE_RANGE, (unsigned)fd, (unsigned)fd,
                       CLOSE_RANGE_CLOEXEC), EPERM);
        errno = 0;
        denied("fcntl cannot clear cached CLOEXEC", fcntl(fd, F_SETFD, 0), EPERM);
        errno = 0;
        int flags = fcntl(fd, F_GETFD);
        check(flags >= 0 && (flags & FD_CLOEXEC),
              "cached descriptor retains CLOEXEC", flags, errno);
    }
    unsigned first = (unsigned)(kgsl < render ? kgsl : render);
    unsigned last = (unsigned)(kgsl > render ? kgsl : render);
    errno = 0;
    denied("range covering both caches blocked",
           syscall(NR_CLOSE_RANGE, first, last, 0), EPERM);

    errno = 0;
    denied("original getppid EACCES preserved", syscall(NR_GETPPID), EACCES);
    errno = 0;
    denied("prefix explicitly rejects execve(NULL) with EPERM",
           syscall(NR_EXECVE, (void *)NULL, (void *)NULL, (void *)NULL), EPERM);
    errno = 0;
    denied("prefix explicitly rejects execveat(NULL) with EPERM",
           syscall(NR_EXECVEAT, AT_FDCWD, (void *)NULL, (void *)NULL,
                   (void *)NULL, 0), EPERM);
    errno = 0;
    denied("prefix explicitly rejects io_uring_setup with EPERM",
           syscall(NR_IO_URING_SETUP, 1, (void *)NULL), EPERM);
    errno = 0;
    denied("prefix explicitly rejects io_uring_enter with EPERM",
           syscall(NR_IO_URING_ENTER, -1, 0, 0, 0, (void *)NULL, 0), EPERM);
    errno = 0;
    denied("prefix explicitly rejects io_uring_register with EPERM",
           syscall(NR_IO_URING_REGISTER, -1, 0, (void *)NULL, 0), EPERM);

    char sent = 'K', received = 0;
    errno = 0;
    ssize_t written = write(pipefds[1], &sent, 1);
    check(written == 1, "ordinary write still works", written, errno);
    errno = 0;
    ssize_t got = read(pipefds[0], &received, 1);
    check(got == 1 && received == sent, "ordinary read still works", got, errno);
    errno = 0;
    int closed = close(pipefds[0]);
    check(closed == 0, "ordinary pipe read fd close still works", closed, errno);
    errno = 0;
    closed = close(pipefds[1]);
    check(closed == 0, "ordinary pipe write fd close still works", closed, errno);

    errno = 0;
    int copy = fcntl(kgsl, F_DUPFD_CLOEXEC, (int)last + 32);
    check(copy >= 0, "normal CLOEXEC duplication is allowed", copy, errno);
    if (copy >= 0) {
        errno = 0;
        denied("duplicate fd does not inherit cached-slot ioctl allowance",
               syscall(NR_IOCTL, copy, (unsigned long)DMD_KGSL_GETPROPERTY,
                       (void *)NULL), EPERM);
        errno = 0;
        long rc = syscall(NR_DUP3, nullfd, copy, O_CLOEXEC);
        check(rc == copy, "dup3 outside cached slots still works", rc, errno);
        errno = 0;
        rc = syscall(NR_CLOSE_RANGE, (unsigned)copy, (unsigned)copy, 0);
        check(rc == 0, "close_range outside cached slots still works", rc, errno);
    }
    errno = 0;
    closed = close(nullfd);
    check(closed == 0, "ordinary device fd close still works", closed, errno);
    after = getproperty(kgsl);
    check(after.result == before_kgsl.result && after.error == before_kgsl.error,
          "KGSL cache remains intact after overwrite attempts", after.result, after.error);
    after = getproperty(render);
    check(after.result == before_render.result && after.error == before_render.error,
          "render cache remains intact after overwrite attempts", after.result, after.error);
    fprintf(stderr, "Kernel policy test: %s (%d failures)\n",
            failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
#if !defined(__aarch64__)
    const int native_aarch64 = 0;
#else
    const int native_aarch64 = 1;
#endif
    if (!native_aarch64) {
        fprintf(stderr, "SKIP: this test installs AArch64 BPF; compile check only on this host\n");
        return 77;
    }
    if (getuid() == 0 || geteuid() == 0) {
        fprintf(stderr, "Run as Gold/the ordinary desktop user, not root\n");
        return 2;
    }
    const char *kgsl = "/dev/kgsl-3d0", *render = "/dev/dri/renderD128";
    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc && !strcmp(argv[i], "--kgsl"))
            kgsl = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--render"))
            render = argv[++i];
        else {
            fprintf(stderr, "Usage: %s [--kgsl /dev/kgsl-3d0] [--render /dev/dri/renderD128]\n", argv[0]);
            return 2;
        }
    }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        perror("fork test child");
        return 1;
    }
    if (child == 0)
        _exit(child_test(kgsl, render));
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 1;
        }
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "Test child terminated by signal %d\n", WTERMSIG(status));
        return 1;
    }
    return WEXITSTATUS(status);
}
