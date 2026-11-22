#define _GNU_SOURCE
#include "kgsl-filter.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/seccomp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KGSL_FD 91
#define RENDER_FD 73
#define REQUEST_TWO UINT32_C(0x40040914)
#define DENY (SECCOMP_RET_ERRNO | EPERM)
#define ORIGINAL_IOCTL (SECCOMP_RET_TRAP | 0x42u)
#define ORIGINAL_EXEC (SECCOMP_RET_ERRNO | EACCES)

/* Exercises original relative jumps and the accumulator's initial zero state.
 * Normal fd operations are permitted here so the new restrictions are tested,
 * The original uses EACCES for exec/io_uring so tests distinguish its decision
 * from the new policy's independent EPERM restrictions. */
static struct sock_filter original_code[] = {
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_AUDIT_ARCH, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_IOCTL, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_IOCTL),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_EXECVE, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_EXEC),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_EXECVEAT, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_EXEC),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_IO_URING_SETUP, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_EXEC),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_IO_URING_ENTER, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_EXEC),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DMD_AARCH64_NR_IO_URING_REGISTER, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, ORIGINAL_EXEC),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 172, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

static struct sock_fprog original = {
    .len = sizeof(original_code) / sizeof(original_code[0]),
    .filter = original_code,
};

static const uint32_t requests[] = {DMD_KGSL_GETPROPERTY, REQUEST_TWO};

/* A small test interpreter, not a kernel BPF validator or filter installer. */
static uint32_t interpret(const struct sock_fprog *program,
                          const struct seccomp_data *data)
{
    uint32_t a = 0;
    size_t pc = 0;
    for (unsigned steps = 0; steps < 10000; ++steps) {
        assert(pc < program->len);
        const struct sock_filter *ins = &program->filter[pc++];
        switch (ins->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            assert(ins->k <= sizeof(*data) - sizeof(uint32_t));
            memcpy(&a, (const unsigned char *)data + ins->k, sizeof(a));
            break;
        case BPF_LD | BPF_W | BPF_IMM:
            a = ins->k;
            break;
        case BPF_JMP | BPF_JA:
            pc += ins->k;
            break;
        case BPF_JMP | BPF_JEQ | BPF_K:
            pc += a == ins->k ? ins->jt : ins->jf;
            break;
        case BPF_JMP | BPF_JGT | BPF_K:
            pc += a > ins->k ? ins->jt : ins->jf;
            break;
        case BPF_JMP | BPF_JGE | BPF_K:
            pc += a >= ins->k ? ins->jt : ins->jf;
            break;
        case BPF_RET | BPF_K:
            return ins->k;
        default:
            assert(!"unsupported instruction in test interpreter");
        }
    }
    assert(!"BPF interpreter did not terminate");
    return 0;
}

static bool reserved(uint32_t fd)
{
    return fd == KGSL_FD || fd == RENDER_FD;
}

static uint32_t oracle(const struct seccomp_data *d)
{
    if (d->arch != DMD_AARCH64_AUDIT_ARCH)
        return SECCOMP_RET_KILL_PROCESS;
    {
        if (d->nr == DMD_AARCH64_NR_EXECVE ||
            d->nr == DMD_AARCH64_NR_EXECVEAT ||
            d->nr == DMD_AARCH64_NR_IO_URING_SETUP ||
            d->nr == DMD_AARCH64_NR_IO_URING_ENTER ||
            d->nr == DMD_AARCH64_NR_IO_URING_REGISTER)
            return DENY;
        const uint32_t a0 = (uint32_t)d->args[0];
        const uint32_t a1 = (uint32_t)d->args[1];
        if (d->nr == DMD_AARCH64_NR_CLOSE && reserved(a0))
            return DENY;
        if (d->nr == DMD_AARCH64_NR_DUP3 && reserved(a1))
            return DENY;
        if (d->nr == DMD_AARCH64_NR_CLOSE_RANGE &&
            ((a0 <= KGSL_FD && a1 >= KGSL_FD) ||
             (a0 <= RENDER_FD && a1 >= RENDER_FD)))
            return DENY;
        if (d->nr == DMD_AARCH64_NR_FCNTL && reserved(a0) && a1 == F_SETFD)
            return DENY;
        if (d->nr == DMD_AARCH64_NR_IOCTL && d->args[0] <= UINT32_MAX &&
            d->args[1] <= UINT32_MAX) {
            if (a0 == KGSL_FD)
                for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
                    if (a1 == requests[i])
                        return SECCOMP_RET_ALLOW;
            if (a0 == RENDER_FD && a1 == DMD_KGSL_GETPROPERTY)
                return SECCOMP_RET_ALLOW;
        }
    }
    return interpret(&original, d);
}

static void check(const struct sock_fprog *program, int nr,
                  uint64_t arg0, uint64_t arg1, uint64_t arg2,
                  uint32_t expected)
{
    struct seccomp_data d = {
        .arch = DMD_AARCH64_AUDIT_ARCH,
        .nr = nr,
        .args = {arg0, arg1, arg2, 0, 0, 0},
    };
    assert(oracle(&d) == expected);
    assert(interpret(program, &d) == expected);
}

static void deterministic_tests(const struct sock_fprog *program)
{
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
        check(program, DMD_AARCH64_NR_IOCTL, KGSL_FD, requests[i], 0x1234,
              SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_IOCTL, RENDER_FD, DMD_KGSL_GETPROPERTY,
          0x5678, SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_IOCTL, RENDER_FD, REQUEST_TWO, 0, ORIGINAL_IOCTL);
    check(program, DMD_AARCH64_NR_IOCTL, 4, DMD_KGSL_GETPROPERTY, 0, ORIGINAL_IOCTL);
    check(program, DMD_AARCH64_NR_IOCTL, KGSL_FD,
          DMD_KGSL_GETPROPERTY ^ UINT32_C(0x10000), 0, ORIGINAL_IOCTL);
    check(program, DMD_AARCH64_NR_IOCTL, KGSL_FD,
          DMD_KGSL_GETPROPERTY | (UINT64_C(1) << 32), 0, ORIGINAL_IOCTL);
    check(program, DMD_AARCH64_NR_IOCTL,
          KGSL_FD | (UINT64_C(1) << 32), DMD_KGSL_GETPROPERTY, 0, ORIGINAL_IOCTL);

    const int fds[] = {KGSL_FD, RENDER_FD};
    for (size_t i = 0; i < 2; ++i) {
        int fd = fds[i];
        check(program, DMD_AARCH64_NR_CLOSE, fd, 0, 0, DENY);
        check(program, DMD_AARCH64_NR_CLOSE,
              (uint64_t)fd | (UINT64_C(0xabcdef) << 32), 0, 0, DENY);
        check(program, DMD_AARCH64_NR_DUP3, 5, fd, 0, DENY);
        check(program, DMD_AARCH64_NR_DUP3, 5,
              (uint64_t)fd | (UINT64_C(1) << 32), O_CLOEXEC, DENY);
        check(program, DMD_AARCH64_NR_DUP3, fd, 5, O_CLOEXEC, SECCOMP_RET_ALLOW);
        check(program, DMD_AARCH64_NR_FCNTL, fd, F_SETFD, 0, DENY);
        check(program, DMD_AARCH64_NR_FCNTL, fd, F_SETFD, FD_CLOEXEC, DENY);
        check(program, DMD_AARCH64_NR_FCNTL, fd,
              F_SETFD | (UINT64_C(1) << 32), 0, DENY);
        check(program, DMD_AARCH64_NR_FCNTL, fd, F_GETFD, 0, SECCOMP_RET_ALLOW);
        check(program, DMD_AARCH64_NR_FCNTL, fd, F_DUPFD, 3, SECCOMP_RET_ALLOW);
        check(program, DMD_AARCH64_NR_CLOSE_RANGE, fd, fd, 0, DENY);
        check(program, DMD_AARCH64_NR_CLOSE_RANGE, 0, fd, 0, DENY);
        check(program, DMD_AARCH64_NR_CLOSE_RANGE, fd, UINT32_MAX, 4, DENY);
    }
    check(program, DMD_AARCH64_NR_CLOSE, 5, 0, 0, SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_DUP3, 5, 6, 0, SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_FCNTL, 5, F_SETFD, 0, SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_CLOSE_RANGE, 0, RENDER_FD - 1, 0,
          SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_CLOSE_RANGE, KGSL_FD + 1, UINT32_MAX, 0,
          SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_CLOSE_RANGE, RENDER_FD + 1, KGSL_FD - 1, 0,
          SECCOMP_RET_ALLOW);
    check(program, DMD_AARCH64_NR_CLOSE_RANGE, 0, UINT32_MAX, 0, DENY);
    check(program, 172, KGSL_FD, DMD_KGSL_GETPROPERTY, 0,
          SECCOMP_RET_ERRNO | ENOSYS);
    check(program, 63, KGSL_FD, 0, 0, SECCOMP_RET_ALLOW);
    const unsigned blocked[] = {
        DMD_AARCH64_NR_EXECVE, DMD_AARCH64_NR_EXECVEAT,
        DMD_AARCH64_NR_IO_URING_SETUP, DMD_AARCH64_NR_IO_URING_ENTER,
        DMD_AARCH64_NR_IO_URING_REGISTER,
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i)
        check(program, (int)blocked[i], 0, 0, 0, DENY);

    struct seccomp_data alternate = {
        .arch = UINT32_C(0xc000003e), /* x86-64 is independently killed */
        .nr = DMD_AARCH64_NR_IOCTL,
        .args = {KGSL_FD, DMD_KGSL_GETPROPERTY, 0, 0, 0, 0},
    };
    assert(interpret(program, &alternate) == SECCOMP_RET_KILL_PROCESS);
}

static uint64_t random_state = UINT64_C(0xa831b72645d0c99f);
static uint64_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static void differential_tests(const struct sock_fprog *program)
{
    const uint64_t values[] = {
        0, 1, 2, 3, 5, KGSL_FD, RENDER_FD, KGSL_FD - 1, KGSL_FD + 1,
        RENDER_FD - 1, RENDER_FD + 1, F_GETFD, F_SETFD,
        DMD_KGSL_GETPROPERTY, REQUEST_TWO, UINT32_MAX,
        UINT64_C(0x100000000) | KGSL_FD,
        UINT64_C(0x100000000) | DMD_KGSL_GETPROPERTY,
        UINT64_C(0xffffffff00000000) | RENDER_FD,
    };
    const int numbers[] = {
        DMD_AARCH64_NR_CLOSE, DMD_AARCH64_NR_DUP3, DMD_AARCH64_NR_FCNTL,
        DMD_AARCH64_NR_CLOSE_RANGE, DMD_AARCH64_NR_IOCTL,
        DMD_AARCH64_NR_EXECVE, DMD_AARCH64_NR_EXECVEAT, 63, 172, -1,
        DMD_AARCH64_NR_IO_URING_SETUP, DMD_AARCH64_NR_IO_URING_ENTER,
        DMD_AARCH64_NR_IO_URING_REGISTER,
    };
    for (unsigned i = 0; i < 100000; ++i) {
        struct seccomp_data d = {0};
        d.arch = i % 101 ? DMD_AARCH64_AUDIT_ARCH : UINT32_C(0x40000028);
        d.nr = numbers[next_random() % (sizeof(numbers) / sizeof(numbers[0]))];
        for (size_t j = 0; j < 6; ++j)
            d.args[j] = values[next_random() % (sizeof(values) / sizeof(values[0]))];
        assert(interpret(program, &d) == oracle(&d));
    }
}

static void validation_tests(void)
{
    struct dmd_kgsl_filter *out = (void *)(uintptr_t)1;
    errno = 0;
    assert(dmd_kgsl_filter_create(NULL, KGSL_FD, RENDER_FD, requests, 2, &out) < 0);
    assert(errno == EINVAL && out == NULL);
    assert(dmd_kgsl_filter_create(&original, 2, RENDER_FD, requests, 2, &out) < 0);
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, KGSL_FD, requests, 2, &out) < 0);
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD, requests, 0, &out) < 0);
    uint32_t wrong_namespace = UINT32_C(0xc0106401);
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD,
                                  &wrong_namespace, 1, &out) < 0);
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD, requests, 2, NULL) < 0);
    uint32_t many[DMD_KGSL_MAX_REQUESTS];
    for (unsigned i = 0; i < DMD_KGSL_MAX_REQUESTS; ++i)
        many[i] = UINT32_C(0xc0180900) + i;
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD,
                                  many, DMD_KGSL_MAX_REQUESTS, &out) == 0);
    struct seccomp_data d = {
        .arch = DMD_AARCH64_AUDIT_ARCH,
        .nr = DMD_AARCH64_NR_IOCTL,
        .args = {KGSL_FD, many[DMD_KGSL_MAX_REQUESTS - 1], 0, 0, 0, 0},
    };
    assert(interpret(&out->program, &d) == SECCOMP_RET_ALLOW);
    dmd_kgsl_filter_free(out);
    out = NULL;
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD,
                                  many, DMD_KGSL_MAX_REQUESTS + 1, &out) < 0);
    struct sock_filter *large = calloc(BPF_MAXINSNS, sizeof(*large));
    assert(large);
    struct sock_fprog too_large = {.filter = large, .len = BPF_MAXINSNS};
    assert(dmd_kgsl_filter_create(&too_large, KGSL_FD, RENDER_FD,
                                  requests, 2, &out) < 0);
    assert(errno == E2BIG && out == NULL);
    free(large);
    dmd_kgsl_filter_free(NULL);

    /* Range checks must also work when the two reserved fd numbers change
     * order. The oracle above deliberately uses the opposite order. */
    assert(dmd_kgsl_filter_create(&original, RENDER_FD, KGSL_FD,
                                  requests, 2, &out) == 0);
    d = (struct seccomp_data){
        .arch = DMD_AARCH64_AUDIT_ARCH,
        .nr = DMD_AARCH64_NR_CLOSE_RANGE,
    };
    d.args[0] = RENDER_FD + 1;
    d.args[1] = KGSL_FD - 1;
    assert(interpret(&out->program, &d) == SECCOMP_RET_ALLOW);
    d.args[0] = d.args[1] = RENDER_FD;
    assert(interpret(&out->program, &d) == DENY);
    d.args[0] = d.args[1] = KGSL_FD;
    assert(interpret(&out->program, &d) == DENY);
    d.nr = DMD_AARCH64_NR_IOCTL;
    d.args[0] = RENDER_FD;
    d.args[1] = REQUEST_TWO;
    assert(interpret(&out->program, &d) == SECCOMP_RET_ALLOW);
    d.args[0] = KGSL_FD;
    assert(interpret(&out->program, &d) == ORIGINAL_IOCTL);
    dmd_kgsl_filter_free(out);
}

static void independent_boundary_tests(void)
{
    /* Even a permissive original filter cannot enable exec, io_uring or
     * alternate architectures after this policy extension. */
    struct sock_filter allow = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog permissive = {.len = 1, .filter = &allow};
    struct dmd_kgsl_filter *out = NULL;
    assert(dmd_kgsl_filter_create(&permissive, KGSL_FD, RENDER_FD,
                                  requests, 2, &out) == 0);
    struct seccomp_data d = {.arch = DMD_AARCH64_AUDIT_ARCH};
    const unsigned blocked[] = {
        DMD_AARCH64_NR_EXECVE, DMD_AARCH64_NR_EXECVEAT,
        DMD_AARCH64_NR_IO_URING_SETUP, DMD_AARCH64_NR_IO_URING_ENTER,
        DMD_AARCH64_NR_IO_URING_REGISTER,
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        d.nr = (int)blocked[i];
        assert(interpret(&out->program, &d) == DENY);
    }
    d.nr = 63;
    assert(interpret(&out->program, &d) == SECCOMP_RET_ALLOW);
    const uint32_t architectures[] = {0, UINT32_C(0xc000003e), UINT32_C(0x40000028)};
    for (size_t i = 0; i < sizeof(architectures) / sizeof(architectures[0]); ++i) {
        d.arch = architectures[i];
        assert(interpret(&out->program, &d) == SECCOMP_RET_KILL_PROCESS);
    }
    dmd_kgsl_filter_free(out);
}

int main(void)
{
    struct sock_filter before[sizeof(original_code) / sizeof(original_code[0])];
    memcpy(before, original_code, sizeof(before));
    struct dmd_kgsl_filter *out = NULL;
    assert(dmd_kgsl_filter_create(&original, KGSL_FD, RENDER_FD,
                                  requests, 2, &out) == 0);
    assert(out && out->original_length == original.len);
    assert(out->program.len == out->prefix_length + original.len);
    assert(memcmp(before, original_code, sizeof(before)) == 0);
    assert(memcmp(out->program.filter + out->prefix_length, before,
                  sizeof(before)) == 0);
    deterministic_tests(&out->program);
    differential_tests(&out->program);
    printf("PASS: prefix %zu, original %u, exact requests/FD guards, "
           "100000 differential cases\n", out->prefix_length, original.len);
    dmd_kgsl_filter_free(out);
    validation_tests();
    independent_boundary_tests();
    puts("PASS: bounds, ownership, namespace, max whitelist, swapped fd order, "
         "independent exec/io_uring/arch boundaries, errors");
    return 0;
}
