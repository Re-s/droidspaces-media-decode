/* MPEG-2 head synthesis (for the description of the realization and design see the header comment for mpeg2_bitstream.c). */
#ifndef DMD_MPEG2_BITSTREAM_H
#define DMD_MPEG2_BITSTREAM_H

#include <stddef.h>
#include <va/va.h>

/* The number of bytes written is returned, 0 = failure/ capacity is insufficient.
 * iq can be NULL (i.e. without quantitative matrix, decoder using default matrix). */
size_t dmd_mpeg2_build_sequence(const VAPictureParameterBufferMPEG2 *pp,
                                const VAIQMatrixBufferMPEG2 *iq,
                                unsigned char *out, size_t out_cap);

/* The number of bytes returned to write failed.
 * Temporal_ref is maintained by the caller in display order (module 1024). */
size_t dmd_mpeg2_build_picture(const VAPictureParameterBufferMPEG2 *pp,
                               unsigned int temporal_ref,
                               unsigned char *out, size_t out_cap);

/* This driver can be used to synthesize the frame for a legitimate stream. 0 = no (caller should return UNIMLEMENTED
 * Let the top fall softly instead of sending a stream that will solve the bad picture. */
int dmd_mpeg2_can_build(const VAPictureParameterBufferMPEG2 *pp);

#endif /* DMD_MPEG2_BITSTREAM_H */
