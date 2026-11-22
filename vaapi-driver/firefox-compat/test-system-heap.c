#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int open_count;
static int open_error;
static int mock_open(const char *path, int flags)
{
    ++open_count;
    assert(flags == (O_RDONLY | O_CLOEXEC));
    if (open_error) {
        errno = open_error;
        return -1;
    }
    return open(path, flags);
}
#define DMD_FIREFOX_HEAP_NO_CONSTRUCTOR
#define DMD_FIREFOX_HEAP_TEST_PATH "/dev/null"
#define DMD_FIREFOX_HEAP_OPEN mock_open
#include "firefox-system-heap.c"

static void reset_cache(void)
{
    if (cache.fd >= 0)
        close(cache.fd);
    cache = (struct heap_cache){ .fd = -1, .error = ENODEV };
    open_count = 0;
    open_error = 0;
}

static void role_tests(void)
{
    char *parent[] = {"firefox", "https://example.test/rdd"};
    char *content[] = {"firefox", "-contentproc", "tab"};
    char *server[] = {"firefox", "-contentproc", "forkserver"};
    char *partial[] = {"firefox", "-contentproc", "rdd-extra"};
    char *not_child[] = {"firefox", "rdd"};
    char *rdd[] = {"firefox", "-contentproc", "-childID", "5",
                   "-sandboxReporter", "8", "rdd"};
    char *misplaced[] = {"firefox", "-contentproc", "rdd", "tab"};
    preopen_system_heap(2, parent);
    preopen_system_heap(3, content);
    preopen_system_heap(3, server);
    preopen_system_heap(3, partial);
    preopen_system_heap(2, not_child);
    preopen_system_heap(4, misplaced);
    assert(open_count == 0);
    errno = 0;
    assert(dmd_firefox_system_heap_dup() == -1 && errno == ENODEV);
    assert(!is_rdd_child(0, NULL));
    preopen_system_heap(7, rdd);
    assert(open_count == 1 && cache.fd >= 0 && cache.owner == getpid());
    assert(fcntl(cache.fd, F_GETFD) & FD_CLOEXEC);
}

static void *duplicate_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 500; ++i) {
        int fd = dmd_firefox_system_heap_dup();
        assert(fd >= 3 && fd != cache.fd);
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        close(fd);
    }
    return NULL;
}

static void owner_and_lifetime_tests(void)
{
    int a = dmd_firefox_system_heap_dup();
    int b = dmd_firefox_system_heap_dup();
    assert(a >= 3 && b >= 3 && a != b);
    close(a);
    assert(fcntl(b, F_GETFD) >= 0);
    close(b);

    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        assert(pthread_create(&threads[i], NULL, duplicate_thread, NULL) == 0);
    for (int i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        errno = 0;
        _exit(dmd_firefox_system_heap_dup() == -1 && errno == ECHILD ? 0 : 1);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int other = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    assert(other >= 0 && other != cache.fd);
    assert(dup2(other, cache.fd) == cache.fd);
    close(other);
    errno = 0;
    assert(dmd_firefox_system_heap_dup() == -1 && errno == ESTALE);
    close(cache.fd);
    errno = 0;
    assert(dmd_firefox_system_heap_dup() == -1 && errno == EBADF);
    cache.fd = -1;
}

int main(void)
{
    reset_cache();
    role_tests();
    owner_and_lifetime_tests();
    reset_cache();
    char *rdd[] = {"firefox", "-contentproc", "rdd"};
    open_error = EACCES;
    preopen_system_heap(3, rdd);
    errno = 0;
    assert(open_count == 1);
    assert(dmd_firefox_system_heap_dup() == -1 && errno == EACCES);
    reset_cache();
    puts("PASS: role, ownership, CLOEXEC, concurrent dup, fork, stale fd, errors");
    return 0;
}
