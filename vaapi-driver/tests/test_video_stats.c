/* Host-only anonymous-memory protocol/lifetime tests. No video devices. */
#include "video_stats.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); exit(1); } } while (0)
static int fail_memfd;
static atomic_uint_fast64_t fake_time = ATOMIC_VAR_INIT(UINT64_C(2000000000));
static atomic_int use_fake_time = ATOMIC_VAR_INIT(1);

int memfd_create(const char *name, unsigned flags)
{
    if (fail_memfd) { errno = EPERM; return -1; }
    return (int)syscall(SYS_memfd_create, name, flags);
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_MONOTONIC && atomic_load(&use_fake_time)) {
        uint64_t t = atomic_fetch_add(&fake_time, 1000);
        ts->tv_sec = (time_t)(t / UINT64_C(1000000000));
        ts->tv_nsec = (long)(t % UINT64_C(1000000000));
        return 0;
    }
    return (int)syscall(SYS_clock_gettime, id, ts);
}

static int stats_fd(void)
{
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != NULL);
    struct dirent *ent;
    int result = -1;
    while ((ent = readdir(d))) {
        char path[128], target[256];
        snprintf(path, sizeof(path), "/proc/self/fd/%.40s", ent->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) continue;
        target[n] = 0;
        if (strstr(target, "/memfd:" DMD_VIDEO_STATS_NAME)) {
            CHECK(result < 0); /* each fixture owns exactly one context */
            result = atoi(ent->d_name);
        }
    }
    closedir(d);
    return result;
}

static uint64_t number(const char *json, const char *name)
{
    char key[128];
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *p = strstr(json, key);
    CHECK(p != NULL);
    char *end;
    uint64_t value = strtoull(p + strlen(key), &end, 10);
    CHECK(end > p + strlen(key));
    CHECK(*end == ',' || *end == '}');
    return value;
}

/* Same pread/retry algorithm as a cross-process monitor. No mapped C payload
 * loads race with the writer; the kernel supplies independent byte copies. */
static int snapshot(int fd, char json[4096], uint64_t *seq)
{
    unsigned char header[32];
    CHECK(pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    CHECK(!memcmp(header, "DMDVID1", 8));
    uint32_t version, capacity, len;
    memcpy(&version, header + 8, 4);
    memcpy(&capacity, header + 12, 4);
    memcpy(seq, header + 16, 8);
    memcpy(&len, header + 24, 4);
    CHECK(version == 1 && capacity == 4096);
    if (*seq & 1) return 0;
    CHECK(len > 1 && len < 4096 - 32);
    CHECK(pread(fd, json, len, 32) == (ssize_t)len);
    uint64_t after;
    CHECK(pread(fd, &after, sizeof(after), 16) == (ssize_t)sizeof(after));
    if (*seq != after || (after & 1)) return 0;
    json[len] = 0;
    CHECK(json[0] == '{' && json[len - 1] == '}');
    for (uint32_t i = 0; i < len; ++i) CHECK((unsigned char)json[i] < 128);
    return 1;
}

static void read_snapshot(int fd, char json[4096], uint64_t *seq)
{
    for (int i = 0; i < 10000; ++i) if (snapshot(fd, json, seq)) return;
    CHECK(0);
}

static void test_disabled_and_failure(void)
{
    CHECK(stats_fd() < 0);
    CHECK(setenv("DMD_VIDEO_STATS", "0", 1) == 0);
    errno = EDOM;
    CHECK(dmd_video_stats_create(1, 0, 0, 0, "test") == NULL);
    CHECK(errno == EDOM && stats_fd() < 0);
    CHECK(unsetenv("DMD_VIDEO_STATS") == 0);
    fail_memfd = 1;
    errno = ERANGE;
    CHECK(dmd_video_stats_create(1, 0, 0, 0, "test") == NULL);
    CHECK(errno == ERANGE && stats_fd() < 0);
    fail_memfd = 0;
    dmd_video_stats_config(NULL, 1, 1, 1, 1);
    dmd_video_stats_hw(NULL);
    dmd_video_stats_output(NULL, 0);
    dmd_video_stats_error(NULL);
    dmd_video_stats_close(NULL);
    dmd_video_stats_unref(NULL);
}

static void test_protocol_and_lifetime(void)
{
    errno = ENOSPC;
    struct dmd_video_stats *s = dmd_video_stats_create(27, 1, 1, 17, "test\"\\\n\xc3\xa9");
    CHECK(s && errno == ENOSPC);
    int fd = stats_fd();
    CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    CHECK((fcntl(fd, F_GET_SEALS) & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) ==
           (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
    CHECK(ftruncate(fd, 0) < 0 && errno == EPERM);
    char json[4096];
    uint64_t seq;
    read_snapshot(fd, json, &seq);
    CHECK(number(json, "pid") == (uint64_t)getpid());
    CHECK(number(json, "context") == 27 && number(json, "width") == 0);
    CHECK(strstr(json, "\"direction\":\"encode\"") && strstr(json, "\"codec\":\"hevc\""));
    CHECK(strstr(json, "\"target_fps\":null"));
    CHECK(strstr(json, "test\\u0022\\u005c\\u000a\\u00c3\\u00a9"));
    CHECK(strstr(json, "\"state\":\"created\""));
    uint64_t initial = seq;
    errno = EDOM;
    dmd_video_stats_config(s, 1920, 1088, 30000, 1001);
    read_snapshot(fd, json, &seq);
    CHECK(number(json, "height") == 1088); /* never guess 1088 means source 1080 */
    CHECK(strstr(json, "\"resolution_kind\":\"driver-output\""));
    CHECK(strstr(json, "\"source_width\":null,\"source_height\":null"));
    dmd_video_stats_config(s, 1920, 1080, 30000, 1001);
    CHECK(errno == EDOM);
    read_snapshot(fd, json, &seq);
    CHECK(seq > initial && number(json, "height") == 1080);
    CHECK(strstr(json, "\"target_fps\":29.970029"));
    initial = seq;
    dmd_video_stats_config(s, 1920, 1080, 30000, 1001);
    read_snapshot(fd, json, &seq);
    CHECK(seq == initial); /* repeated config does not format/publish again */
    dmd_video_stats_hw(s);
    read_snapshot(fd, json, &seq);
    CHECK(number(json, "hw_completed") == 1 && number(json, "output_frames") == 0);
    dmd_video_stats_output(s, 0);
    dmd_video_stats_hw(s);
    dmd_video_stats_output(s, 1);
    dmd_video_stats_reuse(s);
    read_snapshot(fd, json, &seq);
    /* No clock advance/sleep is needed: the final short-clip frame is visible. */
    CHECK(number(json, "hw_completed") == 2 && number(json, "output_frames") == 2);
    CHECK(number(json, "reused_frames") == 2 && number(json, "last_frame_ns") != 0);
    dmd_video_stats_error(s);
    read_snapshot(fd, json, &seq);
    CHECK(number(json, "errors") == 1 && strstr(json, "\"state\":\"error\""));
    dmd_video_stats_ref(s); /* deferred hardware owns this second reference */
    int reader = dup(fd);
    CHECK(reader >= 0);
    dmd_video_stats_close(s);
    CHECK(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
    read_snapshot(reader, json, &seq);
    CHECK(strstr(json, "\"state\":\"closed\""));
    uint64_t closed_seq = seq;
    dmd_video_stats_unref(s); /* context destroyed while deferred IO is alive */
    dmd_video_stats_hw(s);
    dmd_video_stats_output(s, 0);
    dmd_video_stats_error(s);
    read_snapshot(reader, json, &seq);
    CHECK(seq == closed_seq && number(json, "hw_completed") == 2);
    dmd_video_stats_unref(s);
    CHECK(close(reader) == 0 && stats_fd() < 0);
}

struct worker { struct dmd_video_stats *stats; atomic_int *done; };
static void *produce(void *arg)
{
    struct worker *w = arg;
    for (unsigned i = 0; i < 3000; ++i) {
        dmd_video_stats_hw(w->stats);
        dmd_video_stats_output(w->stats, 0);
    }
    dmd_video_stats_unref(w->stats);
    atomic_fetch_add(w->done, 1);
    return NULL;
}

static void test_concurrent_snapshots(void)
{
    struct dmd_video_stats *s = dmd_video_stats_create(8, 0, 4, 32, "concurrent");
    CHECK(s);
    dmd_video_stats_config(s, 640, 360, 0, 0);
    int fd = stats_fd();
    atomic_int done = ATOMIC_VAR_INIT(0);
    struct worker workers[4];
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){dmd_video_stats_ref(s), &done};
        CHECK(pthread_create(&threads[i], NULL, produce, &workers[i]) == 0);
    }
    uint64_t previous = 0, seq;
    unsigned accepted = 0;
    char json[4096];
    while (atomic_load(&done) < 4) {
        if (!snapshot(fd, json, &seq)) continue;
        CHECK(seq >= previous);
        previous = seq;
        CHECK(number(json, "output_frames") <= number(json, "hw_completed"));
        CHECK(number(json, "width") == 640 && strstr(json, "\"target_fps\":null"));
        accepted++;
    }
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    read_snapshot(fd, json, &seq);
    CHECK(number(json, "hw_completed") == 12000 && number(json, "output_frames") == 12000);
    CHECK(accepted > 0);
    dmd_video_stats_close(s);
    dmd_video_stats_unref(s);
    CHECK(stats_fd() < 0);
}

static void benchmark(void)
{
    atomic_store(&use_fake_time, 0);
    struct dmd_video_stats *s = dmd_video_stats_create(1, 0, 0, 0, "benchmark");
    CHECK(s);
    struct timespec a, b;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    unsigned frames = 100000;
    for (unsigned i = 0; i < frames; ++i) {
        dmd_video_stats_hw(s);
        dmd_video_stats_output(s, 0);
    }
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    uint64_t ns = (uint64_t)(b.tv_sec - a.tv_sec) * UINT64_C(1000000000) + b.tv_nsec - a.tv_nsec;
    printf("video stats benchmark: %u frames, 2 snapshots/frame, %.3f us/frame\n", frames,
           (double)ns / frames / 1000.0);
    dmd_video_stats_unref(s);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) { benchmark(); return 0; }
    test_disabled_and_failure();
    test_protocol_and_lifetime();
    test_concurrent_snapshots();
    puts("video stats protocol/lifetime/concurrency tests passed");
    return 0;
}
