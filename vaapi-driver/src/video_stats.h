/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DMD_VIDEO_STATS_H
#define DMD_VIDEO_STATS_H

#include <stdint.h>

#define DMD_VIDEO_STATS_NAME "dmd-video-v1"
#define DMD_VIDEO_STATS_CAPACITY 4096u
#define DMD_VIDEO_STATS_JSON_OFFSET 32u

/* The byte offsets are a public, little-endian Linux ABI, not sizeof(struct).
 * See doc/video-stats-v1.md. Writers serialize internally; readers must retry
 * unless the two sequence reads are equal and even. */
struct dmd_video_stats;

struct dmd_video_stats *dmd_video_stats_create(uint32_t context, int encode,
                                              int codec, int profile, const char *build);
struct dmd_video_stats *dmd_video_stats_ref(struct dmd_video_stats *stats);
void dmd_video_stats_unref(struct dmd_video_stats *stats);
/* Immediately stop publishing and close the context's descriptor. Other
 * references can still be used by already-running, deferred hardware IO. */
void dmd_video_stats_close(struct dmd_video_stats *stats);
void dmd_video_stats_config(struct dmd_video_stats *stats, unsigned width,
                            unsigned height, unsigned fps_num, unsigned fps_den);
void dmd_video_stats_hw(struct dmd_video_stats *stats);
void dmd_video_stats_output(struct dmd_video_stats *stats, int reused);
/* A decoded surface can retain the legacy VA success path despite a known
 * kernel error flag. Do not turn that into a successful telemetry output. */
void dmd_video_stats_decoded(struct dmd_video_stats *stats, unsigned width,
                             unsigned height, int hardware_error);
/* READY without fresh hardware pixels in a legacy AV1 recovery path. This
 * deliberately does not increment output_frames or hw_completed. */
void dmd_video_stats_reuse(struct dmd_video_stats *stats);
void dmd_video_stats_error(struct dmd_video_stats *stats);

#endif
