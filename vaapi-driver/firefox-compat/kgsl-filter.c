/* SPDX-License-Identifier: MIT
 * Pure classic-BPF builder. No syscall/open/ioctl interposition or installation.
 */
#include "kgsl-filter.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/seccomp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PREFIX_MAX 512u
#define FIXUP_MAX 256u

enum label {
    L_CLOSE,
    L_DUP3,
    L_CLOSE_RANGE,
    L_RANGE_RENDER,
    L_FCNTL,
    L_SETFD,
    L_IOCTL,
    L_KGSL_REQUEST,
    L_RENDER_REQUEST,
    L_ALLOW,
    L_DENY,
    L_KILL,
    L_ORIGINAL,
    L_COUNT
};

struct fixup {
    size_t at;
    enum label target;
};

struct builder {
    struct sock_filter instructions[PREFIX_MAX];
    size_t length;
    struct fixup fixups[FIXUP_MAX];
    size_t fixup_count;
    size_t labels[L_COUNT];
    int error;
};

static void emit(struct builder *b, unsigned short code,
                 unsigned char jt, unsigned char jf, uint32_t k)
{
    if (b->error)
        return;
    if (b->length == PREFIX_MAX) {
        b->error = E2BIG;
        return;
    }
    b->instructions[b->length++] = (struct sock_filter){code, jt, jf, k};
}

static void mark(struct builder *b, enum label label)
{
    b->labels[label] = b->length;
}

static void jump(struct builder *b, enum label target)
{
    if (b->error)
        return;
    if (b->fixup_count == FIXUP_MAX || b->length == PREFIX_MAX) {
        b->error = E2BIG;
        return;
    }
    b->fixups[b->fixup_count++] = (struct fixup){b->length, target};
    emit(b, BPF_JMP | BPF_JA, 0, 0, 0);
}

/* Conditional offsets stay 0/1. A following JA supplies a 32-bit forward
 * branch, so adding requests cannot silently overflow 8-bit jt/jf fields. */
static void branch(struct builder *b, unsigned short operation, uint32_t k,
                   enum label target, bool when_true)
{
    emit(b, BPF_JMP | operation | BPF_K,
         when_true ? 0 : 1, when_true ? 1 : 0, k);
    jump(b, target);
}

static void load_word(struct builder *b, size_t offset)
{
    emit(b, BPF_LD | BPF_W | BPF_ABS, 0, 0, (uint32_t)offset);
}

static void load_arg_low(struct builder *b, unsigned index)
{
    load_word(b, offsetof(struct seccomp_data, args) + index * sizeof(uint64_t));
}

static void load_arg_high(struct builder *b, unsigned index)
{
    load_word(b, offsetof(struct seccomp_data, args) +
                   index * sizeof(uint64_t) + sizeof(uint32_t));
}

static int resolve(struct builder *b)
{
    if (b->error) {
        errno = b->error;
        return -1;
    }
    for (size_t i = 0; i < b->fixup_count; ++i) {
        const struct fixup *f = &b->fixups[i];
        size_t target = b->labels[f->target];
        if (target == SIZE_MAX || target <= f->at || target > b->length) {
            errno = EINVAL;
            return -1;
        }
        b->instructions[f->at].k = (uint32_t)(target - f->at - 1);
    }
    return 0;
}

int dmd_kgsl_filter_create(const struct sock_fprog *original,
                           int kgsl_fd, int render_fd,
                           const uint32_t *requests, size_t request_count,
                           struct dmd_kgsl_filter **result)
{
    if (!result) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (!original || !original->filter || !original->len ||
        original->len > BPF_MAXINSNS || kgsl_fd < 3 || render_fd < 3 ||
        kgsl_fd == render_fd || !requests || !request_count ||
        request_count > DMD_KGSL_MAX_REQUESTS) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < request_count; ++i) {
        /* Do not accidentally extend a different ioctl namespace. Full command
         * values, including direction and size, are matched below. */
        if (((requests[i] >> 8) & 0xffu) != 0x09u) {
            errno = EINVAL;
            return -1;
        }
    }

    struct builder b = {0};
    for (size_t i = 0; i < L_COUNT; ++i)
        b.labels[i] = SIZE_MAX;

    load_word(&b, offsetof(struct seccomp_data, arch));
    branch(&b, BPF_JEQ, DMD_AARCH64_AUDIT_ARCH, L_KILL, false);
    load_word(&b, offsetof(struct seccomp_data, nr));
    /* These are explicit restrictions of this new policy, not assumptions
     * about the original program. Filters survive exec, while CLOEXEC would
     * discard the protected FDs; io_uring could bypass ordinary close guards. */
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_EXECVE, L_DENY, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_EXECVEAT, L_DENY, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_IO_URING_SETUP, L_DENY, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_IO_URING_ENTER, L_DENY, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_IO_URING_REGISTER, L_DENY, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_CLOSE, L_CLOSE, true);
    /* AArch64 has no dup2 syscall. glibc dup2 uses dup3; compat architectures
     * are killed above. */
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_DUP3, L_DUP3, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_CLOSE_RANGE, L_CLOSE_RANGE, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_FCNTL, L_FCNTL, true);
    branch(&b, BPF_JEQ, DMD_AARCH64_NR_IOCTL, L_IOCTL, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_CLOSE);
    /* The kernel truncates fd/cmd/range arguments to 32 bits. Guard the low
     * words even when a raw caller supplies a nonzero upper register half. */
    load_arg_low(&b, 0);
    branch(&b, BPF_JEQ, (uint32_t)kgsl_fd, L_DENY, true);
    branch(&b, BPF_JEQ, (uint32_t)render_fd, L_DENY, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_DUP3);
    load_arg_low(&b, 1);
    branch(&b, BPF_JEQ, (uint32_t)kgsl_fd, L_DENY, true);
    branch(&b, BPF_JEQ, (uint32_t)render_fd, L_DENY, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_CLOSE_RANGE);
    load_arg_low(&b, 0);
    branch(&b, BPF_JGT, (uint32_t)kgsl_fd, L_RANGE_RENDER, true);
    load_arg_low(&b, 1);
    branch(&b, BPF_JGE, (uint32_t)kgsl_fd, L_DENY, true);
    mark(&b, L_RANGE_RENDER);
    load_arg_low(&b, 0);
    branch(&b, BPF_JGT, (uint32_t)render_fd, L_ORIGINAL, true);
    load_arg_low(&b, 1);
    branch(&b, BPF_JGE, (uint32_t)render_fd, L_DENY, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_FCNTL);
    load_arg_low(&b, 0);
    branch(&b, BPF_JEQ, (uint32_t)kgsl_fd, L_SETFD, true);
    branch(&b, BPF_JEQ, (uint32_t)render_fd, L_SETFD, true);
    jump(&b, L_ORIGINAL);
    mark(&b, L_SETFD);
    load_arg_low(&b, 1);
    branch(&b, BPF_JEQ, F_SETFD, L_DENY, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_IOCTL);
    /* ALLOW is stricter than kernel truncation: both complete 64-bit syscall
     * arguments must have zero high words and match a positive reserved fd
     * and a complete, explicitly supplied 32-bit ioctl command. */
    load_arg_high(&b, 0);
    branch(&b, BPF_JEQ, 0, L_ORIGINAL, false);
    load_arg_high(&b, 1);
    branch(&b, BPF_JEQ, 0, L_ORIGINAL, false);
    load_arg_low(&b, 0);
    branch(&b, BPF_JEQ, (uint32_t)kgsl_fd, L_KGSL_REQUEST, true);
    branch(&b, BPF_JEQ, (uint32_t)render_fd, L_RENDER_REQUEST, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_KGSL_REQUEST);
    load_arg_low(&b, 1);
    for (size_t i = 0; i < request_count; ++i)
        branch(&b, BPF_JEQ, requests[i], L_ALLOW, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_RENDER_REQUEST);
    load_arg_low(&b, 1);
    branch(&b, BPF_JEQ, DMD_KGSL_GETPROPERTY, L_ALLOW, true);
    jump(&b, L_ORIGINAL);

    mark(&b, L_ALLOW);
    emit(&b, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);
    mark(&b, L_DENY);
    emit(&b, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | EPERM);
    mark(&b, L_KILL);
    emit(&b, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
    mark(&b, L_ORIGINAL);
    /* Prefix touches only A. Restore the original entry state; leave X and
     * scratch memory untouched. All original relative branch offsets survive
     * because the original instruction array is copied without rewriting. */
    emit(&b, BPF_LD | BPF_W | BPF_IMM, 0, 0, 0);
    if (resolve(&b) < 0)
        return -1;
    if (b.length > (size_t)BPF_MAXINSNS - original->len) {
        errno = E2BIG;
        return -1;
    }

    struct dmd_kgsl_filter *out = calloc(1, sizeof(*out));
    if (!out)
        return -1;
    size_t length = b.length + original->len;
    out->program.filter = malloc(length * sizeof(struct sock_filter));
    if (!out->program.filter) {
        free(out);
        return -1;
    }
    memcpy(out->program.filter, b.instructions,
           b.length * sizeof(struct sock_filter));
    memcpy(out->program.filter + b.length, original->filter,
           original->len * sizeof(struct sock_filter));
    out->program.len = (unsigned short)length;
    out->prefix_length = b.length;
    out->original_length = original->len;
    *result = out;
    return 0;
}

void dmd_kgsl_filter_free(struct dmd_kgsl_filter *filter)
{
    if (!filter)
        return;
    free(filter->program.filter);
    free(filter);
}
