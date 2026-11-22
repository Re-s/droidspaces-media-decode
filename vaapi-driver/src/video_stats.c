/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "video_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static const char protocol_marker[] __attribute__((used)) = "DMD_VIDEO_STATS_PROTOCOL=1";
static atomic_uint_fast64_t next_instance = ATOMIC_VAR_INIT(1);

struct stats_header {
    char magic[8];
    uint32_t version, capacity;
    _Atomic uint64_t seq;
    _Atomic uint32_t json_len;
    uint32_t reserved;
};
_Static_assert(sizeof(struct stats_header) == DMD_VIDEO_STATS_JSON_OFFSET, "stats header size");
_Static_assert(__builtin_offsetof(struct stats_header, seq) == 16, "stats seq offset");
_Static_assert(__builtin_offsetof(struct stats_header, json_len) == 24, "stats length offset");

struct dmd_video_stats {
    atomic_uint refs;
    pthread_mutex_t lock;
    struct stats_header *map;
    int fd, pid, uid, encode, codec, profile, closed;
    uint32_t context;
    uint64_t started_ns, instance, last_frame_ns, sequence;
    uint64_t hw_completed, output_frames, reused_frames, errors;
    unsigned width, height, fps_num, fps_den;
    const char *state;
    char build[512]; /* already escaped, ASCII JSON string contents */
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static const char *codec_name(int codec)
{
    static const char *const names[] = { "h264", "hevc", "vp9", "vp8", "av1", "mpeg2" };
    return (unsigned)codec < sizeof(names) / sizeof(names[0]) ? names[codec] : "unknown";
}

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) ++*value;
}

/* Caller holds lock. No write/pwrite or allocation on the frame path. Each
 * completion updates the small anonymous mapping, including the last frame
 * before a pause (an event-throttled snapshot could miss it indefinitely).
 * Atomic sequence fences also order
 * the byte payload on AArch64, where release of the odd value alone is not
 * sufficient to order following payload writes. */
static void publish(struct dmd_video_stats *s, uint64_t now)
{
    if (!s->map || s->closed) return;
    char fps[64];
    if (s->fps_num && s->fps_den)
        snprintf(fps, sizeof(fps), "%u.%06u", s->fps_num / s->fps_den,
                 (unsigned)((uint64_t)(s->fps_num % s->fps_den) * 1000000 / s->fps_den));
    else
        strcpy(fps, "null");
    char json[DMD_VIDEO_STATS_CAPACITY - DMD_VIDEO_STATS_JSON_OFFSET];
    int len = snprintf(json, sizeof(json),
        "{\"pid\":%d,\"uid\":%d,\"context\":%" PRIu32 ","
        "\"session_id\":\"%016" PRIx64 "-%016" PRIx64 "\",\"started_ns\":%" PRIu64 ","
        "\"build\":\"%s\","
        "\"direction\":\"%s\",\"codec\":\"%s\",\"profile\":%d,"
        "\"width\":%u,\"height\":%u,\"resolution_kind\":\"%s\","
        "\"source_width\":null,\"source_height\":null,\"hw_completed\":%" PRIu64 ","
        "\"output_frames\":%" PRIu64 ",\"reused_frames\":%" PRIu64 ","
        "\"errors\":%" PRIu64 ",\"updated_ns\":%" PRIu64 ","
        "\"last_frame_ns\":%" PRIu64 ",\"target_fps\":%s,"
        "\"target_fps_num\":%u,\"target_fps_den\":%u,\"state\":\"%s\"}",
        s->pid, s->uid, s->context, s->started_ns, s->instance, s->started_ns, s->build,
        s->encode ? "encode" : "decode", codec_name(s->codec), s->profile,
        s->width, s->height, s->width && s->height ? "driver-output" : "unknown",
        s->hw_completed, s->output_frames, s->reused_frames,
        s->errors, now, s->last_frame_ns, fps, s->fps_num, s->fps_den, s->state);
    if (len < 0 || (size_t)len >= sizeof(json)) return;
    uint64_t seq = s->sequence;
    atomic_store_explicit(&s->map->seq, seq + 1, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    memcpy((char *)s->map + DMD_VIDEO_STATS_JSON_OFFSET, json, (size_t)len);
    atomic_store_explicit(&s->map->json_len, (uint32_t)len, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->map->seq, seq + 2, memory_order_release);
    s->sequence = seq + 2;
}

struct dmd_video_stats *dmd_video_stats_create(uint32_t context, int encode,
                                              int codec, int profile, const char *build)
{
    int saved = errno;
    const char *setting = getenv("DMD_VIDEO_STATS");
    if (setting && !strcmp(setting, "0")) return NULL;
    struct dmd_video_stats *s = calloc(1, sizeof(*s));
    if (!s) { errno = saved; return NULL; }
    s->fd = -1;
    if (pthread_mutex_init(&s->lock, NULL) != 0) goto fail;
    s->fd = memfd_create(DMD_VIDEO_STATS_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    /* No fchmod: Firefox RDD permits shared-memory creation but can SIGSYS
     * on fchmod, even on an anonymous descriptor. /proc fd access remains
     * subject to Linux ptrace/namespace permissions. */
    if (s->fd < 0 || ftruncate(s->fd, DMD_VIDEO_STATS_CAPACITY) != 0) goto fail_lock;
    s->map = mmap(NULL, DMD_VIDEO_STATS_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED, s->fd, 0);
    if (s->map == MAP_FAILED) { s->map = NULL; goto fail_lock; }
    if (!atomic_is_lock_free(&s->map->seq)) goto fail_map;
    /* Readers cannot truncate a descriptor acquired through /proc. A seal
     * failure is optional: do not create partially supported telemetry. */
    if (fcntl(s->fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
        goto fail_map;
    memcpy(s->map->magic, "DMDVID1", 8);
    s->map->version = 1;
    s->map->capacity = DMD_VIDEO_STATS_CAPACITY;
    atomic_init(&s->map->seq, 0);
    atomic_init(&s->map->json_len, 0);
    atomic_init(&s->refs, 1);
    s->pid = (int)getpid();
    s->uid = (int)getuid();
    s->context = context;
    s->encode = encode != 0;
    s->codec = codec;
    s->profile = profile;
    s->started_ns = now_ns();
    s->instance = atomic_fetch_add_explicit(&next_instance, 1, memory_order_relaxed);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(build ? build : "unknown");
         *p && n + 6 < sizeof(s->build); ++p) {
        if (*p < 32 || *p >= 127 || *p == '"' || *p == '\\') {
            snprintf(s->build + n, sizeof(s->build) - n, "\\u%04x", (unsigned)*p);
            n += 6;
        } else s->build[n++] = (char)*p;
    }
    s->build[n] = 0;
    s->state = "created";
    publish(s, s->started_ns);
    errno = saved;
    return s;
fail_map:
    munmap(s->map, DMD_VIDEO_STATS_CAPACITY);
fail_lock:
    if (s->fd >= 0) close(s->fd);
    pthread_mutex_destroy(&s->lock);
fail:
    free(s);
    errno = saved;
    return NULL;
}

struct dmd_video_stats *dmd_video_stats_ref(struct dmd_video_stats *s)
{
    if (s) atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
    return s;
}

void dmd_video_stats_close(struct dmd_video_stats *s)
{
    if (!s) return;
    int saved = errno;
    pthread_mutex_lock(&s->lock);
    if (!s->closed) {
        s->state = "closed";
        publish(s, now_ns());
        s->closed = 1;
        if (s->map) munmap(s->map, DMD_VIDEO_STATS_CAPACITY);
        s->map = NULL;
        if (s->fd >= 0) close(s->fd);
        s->fd = -1;
    }
    pthread_mutex_unlock(&s->lock);
    errno = saved;
}

void dmd_video_stats_unref(struct dmd_video_stats *s)
{
    if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1) return;
    dmd_video_stats_close(s);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void dmd_video_stats_config(struct dmd_video_stats *s, unsigned w, unsigned h,
                            unsigned fps_num, unsigned fps_den)
{
    if (!s) return;
    int saved = errno;
    pthread_mutex_lock(&s->lock);
    if (!s->closed && (s->width != w || s->height != h ||
        s->fps_num != fps_num || s->fps_den != fps_den)) {
        s->width = w;
        s->height = h;
        s->fps_num = fps_num;
        s->fps_den = fps_den;
        publish(s, now_ns());
    }
    pthread_mutex_unlock(&s->lock);
    errno = saved;
}

static void event(struct dmd_video_stats *s, int kind, int reused)
{
    if (!s) return;
    int saved = errno;
    pthread_mutex_lock(&s->lock);
    if (!s->closed) {
        uint64_t now = now_ns();
        if (kind == 0) {
            increment(&s->hw_completed);
            s->last_frame_ns = now;
            s->state = "active";
        } else if (kind == 1) {
            increment(&s->output_frames);
            if (reused) increment(&s->reused_frames);
            s->last_frame_ns = now;
            s->state = "active";
        } else if (kind == 2) {
            increment(&s->reused_frames);
        } else {
            increment(&s->errors);
            s->state = "error";
        }
        publish(s, now);
    }
    pthread_mutex_unlock(&s->lock);
    errno = saved;
}

void dmd_video_stats_hw(struct dmd_video_stats *s) { event(s, 0, 0); }
void dmd_video_stats_output(struct dmd_video_stats *s, int reused) { event(s, 1, reused); }
void dmd_video_stats_decoded(struct dmd_video_stats *s, unsigned w, unsigned h, int error)
{
    dmd_video_stats_config(s, w, h, 0, 0);
    if (!error) dmd_video_stats_output(s, 0);
}
void dmd_video_stats_reuse(struct dmd_video_stats *s) { event(s, 2, 0); }
void dmd_video_stats_error(struct dmd_video_stats *s) { event(s, 3, 0); }
