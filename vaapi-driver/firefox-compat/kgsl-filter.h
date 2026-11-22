/* SPDX-License-Identifier: MIT */
#ifndef DMD_KGSL_FILTER_H
#define DMD_KGSL_FILTER_H

#include <linux/filter.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMD_KGSL_MAX_REQUESTS 64u
#define DMD_KGSL_GETPROPERTY UINT32_C(0xc0180902)

/* The builder is host-testable, but generated syscall numbers and argument
 * layout are for Linux AArch64 little-endian only. */
#define DMD_AARCH64_AUDIT_ARCH UINT32_C(0xc00000b7)
#define DMD_AARCH64_NR_DUP3 24u
#define DMD_AARCH64_NR_FCNTL 25u
#define DMD_AARCH64_NR_IOCTL 29u
#define DMD_AARCH64_NR_CLOSE 57u
#define DMD_AARCH64_NR_EXECVE 221u
#define DMD_AARCH64_NR_EXECVEAT 281u
#define DMD_AARCH64_NR_IO_URING_SETUP 425u
#define DMD_AARCH64_NR_IO_URING_ENTER 426u
#define DMD_AARCH64_NR_IO_URING_REGISTER 427u
#define DMD_AARCH64_NR_CLOSE_RANGE 436u

struct dmd_kgsl_filter {
    struct sock_fprog program;
    /* Original instructions begin exactly here, byte-for-byte unchanged.
     * The final prefix instruction restores the BPF accumulator to zero. */
    size_t prefix_length;
    unsigned short original_length;
};

/* Returns 0 and a new owned object, or -1 with errno and *result == NULL.
 *
 * The caller must invoke this only for the intended RDD sandbox installation,
 * after validating and reserving two distinct CLOEXEC character-device FDs.
 * This function neither opens devices nor installs filters. It adds explicit
 * KGSL ioctl allowances and FD-slot restrictions; it is a NEW sandbox policy.
 *
 * The prefix explicitly rejects execve/execveat and io_uring with EPERM, and
 * kills non-AArch64 syscall architectures. Other original decisions remain.
 */
int dmd_kgsl_filter_create(const struct sock_fprog *original,
                           int kgsl_fd, int render_fd,
                           const uint32_t *requests, size_t request_count,
                           struct dmd_kgsl_filter **result);
void dmd_kgsl_filter_free(struct dmd_kgsl_filter *filter);

#ifdef __cplusplus
}
#endif
#endif
