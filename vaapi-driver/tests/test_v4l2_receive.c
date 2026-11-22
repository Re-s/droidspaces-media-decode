/* Exercise session receive ordering, wait semantics and buffer ownership. */
#include "dmd_v4l2_session_private.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint8_t pixels[DMD_V4L2_MAX_CAP][256 * 256 * 3 / 2];
static int receive_result, receive_index, receive_format_change;
static uint64_t receive_unit;
static int receive_calls, zero_wait_calls, positive_wait_calls;
static int release_calls, release_error;

/* Inspect the actual backend timeout instead of relying on wall-clock
 * timing that could pass accidentally on a fast machine. */
int dmd_v4l2_recv(struct dmd_v4l2_dec *decoder, uint8_t **output,
                  size_t *length, uint64_t *timestamp, int *index, int wait_ms)
{
    receive_calls++;
    if (wait_ms == 0) zero_wait_calls++;
    if (wait_ms > 0) positive_wait_calls++;
    if (receive_result < 0) errno = EIO;
    if (receive_format_change) {
        decoder->fmt_dirty = 1;
        decoder->w = decoder->stride = decoder->crop_w = 256;
    }
    if (receive_result != 1) return receive_result;
    *output = pixels[receive_index];
    *length = (size_t)decoder->stride * decoder->slice_height * 3 / 2;
    *timestamp = receive_unit * 1000;
    *index = receive_index;
    return 1;
}

int dmd_v4l2_release(struct dmd_v4l2_dec *decoder, int index)
{
    (void)decoder;
    assert(index >= 0 && index < DMD_V4L2_MAX_CAP);
    release_calls++;
    if (!release_error) return 0;
    errno = EIO;
    return -1;
}

static void init_session(struct dmd_session *session)
{
    memset(session, 0, sizeof(*session));
    session->dec.fd = 7;
    session->dec.cap_ready = 1;
    session->dec.w = session->dec.h = 128;
    session->dec.stride = session->dec.slice_height = 128;
    session->dec.crop_w = session->dec.crop_h = 128;
    session->io_timeout_ms = 80;
    dmd_session_publish_format_if_needed(session);
    receive_calls = zero_wait_calls = positive_wait_calls = 0;
    release_calls = release_error = 0;
    receive_result = receive_index = receive_format_change = 0;
    receive_unit = 0;
}

static void test_empty_try_does_not_wait(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    assert(dmd_session_try_next_frame(&session, &frame) == DMD_ERR_TIMEOUT);
    assert(receive_calls == 1 && zero_wait_calls == 1);
    assert(positive_wait_calls == 0 && session.frames_recv == 0);
    assert(dmd_session_last_error_code(&session) == DMD_OK);
}

static void test_pending_frames_precede_backend(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    assert(dmd_session_pending_push(&session, pixels[2], 24576, 1000, 2) == 0);
    assert(dmd_session_pending_push(&session, pixels[3], 24576, 2000, 3) == 0);
    receive_result = 1;
    receive_unit = 3;
    receive_index = 4;
    for (unsigned unit = 1; unit <= 3; unit++) {
        assert(dmd_session_try_next_frame(&session, &frame) == DMD_OK);
        assert(frame.unit_seq == unit && frame.seq == unit - 1);
        assert(frame.shm_slot == (int)unit + 1);
        assert(receive_calls == (unit == 3 ? 1 : 0));
        assert(session.held[frame.shm_slot] == 1);
        assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
        assert(session.held[unit + 1] == 0);
    }
    assert(session.frames_recv == 3 && session.pend_count == 0);
    assert(release_calls == 3);
}

static void test_pending_capacity_and_wrap(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    for (int round = 0; round < 3; round++) {
        for (int index = 0; index < DMD_V4L2_MAX_CAP; index++)
            assert(dmd_session_pending_push(&session, pixels[index], 24576,
                                            (uint64_t)(index + 1) * 1000,
                                            index) == 0);
        assert(dmd_session_pending_push(&session, pixels[0], 24576, 1000, 0) < 0);
        for (int index = 0; index < DMD_V4L2_MAX_CAP; index++) {
            assert(dmd_session_try_next_frame(&session, &frame) == DMD_OK);
            assert(frame.unit_seq == (unsigned)index + 1);
            assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
        }
    }
    assert(receive_calls == 0 && session.pend_count == 0);
}

static void test_last_frame_and_release_once(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    receive_result = 1;
    receive_unit = 7;
    receive_index = 5;
    assert(dmd_session_try_next_frame(&session, &frame) == DMD_OK);
    assert(frame.data == pixels[5] && frame.unit_seq == 7);
    assert(frame.width == 128 && frame.crop_right == 127);
    release_error = 1;
    assert(dmd_session_release_frame(&session, &frame) == DMD_ERR_IO);
    assert(frame.data == pixels[5] && session.held[5] == 1);
    release_error = 0;
    assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
    assert(frame.data == NULL && frame.shm_slot == -1 && frame.size == 0);
    assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
    assert(release_calls == 2 && session.held[5] == 0);
}

static void test_format_published_before_frame(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    session.held[8] = 1;
    receive_result = 1;
    receive_format_change = 1;
    receive_unit = 9;
    receive_index = 9;
    assert(dmd_session_try_next_frame(&session, &frame) == DMD_OK);
    assert(frame.width == 256 && frame.stride == 256);
    assert(frame.crop_right == 255 && session.fmt.buf_width == 256);
    assert(session.held[8] == 0 && session.held[9] == 1);
    assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
}

static void test_error_eos_and_invalid_arguments(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    init_session(&session);
    assert(dmd_session_try_next_frame(NULL, &frame) == DMD_ERR_INVAL);
    assert(dmd_session_try_next_frame(&session, NULL) == DMD_ERR_INVAL);
    assert(receive_calls == 0);
    receive_result = -1;
    assert(dmd_session_try_next_frame(&session, &frame) == DMD_ERR_IO);
    assert(dmd_session_last_error_code(&session) == DMD_ERR_IO);
    assert(strstr(dmd_session_last_error(&session), "fd=7") != NULL);
    receive_result = 2;
    assert(dmd_session_try_next_frame(&session, &frame) == DMD_EOS);
    assert(session.eos == 1 && session.frames_recv == 0 && release_calls == 0);
}

static void test_explicit_wait_semantics(void)
{
    struct dmd_session session;
    struct dmd_frame frame;
    const int timeouts[] = {60, 0, -1};
    for (unsigned index = 0; index < sizeof(timeouts) / sizeof(timeouts[0]); index++) {
        init_session(&session);
        assert(dmd_session_next_frame(&session, &frame,
                                       timeouts[index]) == DMD_ERR_TIMEOUT);
        assert(receive_calls > 0 && positive_wait_calls == receive_calls);
        assert(zero_wait_calls == 0);
        receive_result = 1;
        receive_unit = 11;
        assert(dmd_session_next_frame(&session, &frame, timeouts[index]) == DMD_OK);
        assert(frame.unit_seq == 11);
        assert(dmd_session_release_frame(&session, &frame) == DMD_OK);
    }
}

int main(void)
{
    test_empty_try_does_not_wait();
    test_pending_frames_precede_backend();
    test_pending_capacity_and_wrap();
    test_last_frame_and_release_once();
    test_format_published_before_frame();
    test_error_eos_and_invalid_arguments();
    test_explicit_wait_semantics();
    puts("session receive tests passed: nonblocking, ordering, tail frame, format, release, errors and waits");
    return 0;
}
