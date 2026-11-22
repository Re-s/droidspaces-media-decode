/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DMD_ENCODE_H
#define DMD_ENCODE_H

#include <va/va.h>

struct dmd_surface;
struct dmd_buffer;
struct dmd_encode;
struct dmd_video_stats;

/* A synchronous, stateful encoder.  The caller serializes access and keeps
 * source/coded storage alive until end returns.  Sequence parameters persist;
 * picture and slice parameters belong to one begin/render/end transaction.
 * Only progressive NV12, CBR/VBR, one slice and an I/P GOP are supported.
 * Firmware generates the codec headers and chooses coding tools and QP under
 * rate control; packed headers and exact application-selected QP are not used.
 */
struct dmd_encode *dmd_encode_create(VAProfile profile, unsigned width,
                                      unsigned height, unsigned rate_control);
void dmd_encode_destroy(struct dmd_encode *enc);
void dmd_encode_set_video_stats(struct dmd_encode *enc, struct dmd_video_stats *stats);
/* End a rejected VA transaction which never reached end().  This keeps the
 * sequence/session alive; it cannot repair a backend which failed during IO. */
void dmd_encode_abort(struct dmd_encode *enc);
VAStatus dmd_encode_begin(struct dmd_encode *enc, VASurfaceID source);
VAStatus dmd_encode_render(struct dmd_encode *enc, VABufferType type,
                            const void *data, unsigned size,
                            unsigned num_elements);
VABufferID dmd_encode_coded_buffer(const struct dmd_encode *enc);
VAStatus dmd_encode_end(struct dmd_encode *enc,
                         const struct dmd_surface *source,
                         struct dmd_buffer *coded);

#endif
