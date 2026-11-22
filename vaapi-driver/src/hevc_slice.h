/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DMD_HEVC_SLICE_H
#define DMD_HEVC_SLICE_H
#include <stddef.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_dec_hevc.h>

/* The generated SPS has no SPS-resident short-term sets. Replace only the
 * source slice's ST-RPS syntax, keeping its remaining header and CABAC data.
 * Input starts at the two-byte NAL header (no Annex B prefix); output includes
 * a four-byte Annex B prefix. Returns 0 or -1 with errno. No hardware calls. */
int dmd_hevc_rewrite_slice(const VAPictureParameterBufferHEVC *picture,
                            const VASliceParameterBufferHEVC *slice,
                            const uint8_t *input, size_t input_size,
                            uint8_t *output, size_t capacity, size_t *output_size);

/* Assemble slices from one picture. Offsets are absolute in input, after the
 * caller has associated each VA slice parameter with its data buffer. Padding
 * outside the declared slices is not copied. No partial output is committed
 * through output_size on error. */
int dmd_hevc_rewrite_slices(const VAPictureParameterBufferHEVC *picture,
                            const VASliceParameterBufferHEVC *slices,
                            const size_t *offsets, unsigned slice_count,
                            const uint8_t *input, size_t input_size,
                            uint8_t *output, size_t capacity, size_t *output_size);
#endif
