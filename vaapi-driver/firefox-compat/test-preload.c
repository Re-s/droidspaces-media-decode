#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argv;
    assert(argc >= 2);
    int (*dup_heap)(void) = (int (*)(void))dlsym(RTLD_DEFAULT,
                                               "dmd_firefox_system_heap_dup");
    assert(dup_heap);
    int fd = dup_heap();
    const char *expected=getenv("DMD_TEST_EXPECT_OPEN");
    if (expected && strcmp(expected,"1")==0) {
        assert(fd >= 3);
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        close(fd);
    } else {
        assert(fd == -1 && errno == ENODEV);
    }
    puts("PASS: GNU loader constructor role detection");
    return 0;
}
