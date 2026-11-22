# Known limitations

## P1: Existing stream-end and reset presentation issues

Some short HEVC loop/seek fixtures show old pixels near stream boundaries with
both the original driver and the earlier reuse candidate. This change addresses
submission starvation and stale writes to recycled targets; it does not claim
to complete the EOS/reset presentation contract for every browser and codec.
The steady livestream result is not a substitute for complete boundary testing.

## P2: Idle wakeups and unmeasured power impact

The existing 2 ms no-delivery sleep remains. With no pending input, both versions
wake to inspect context state. With pending input but no ready output, the new
nonblocking receiver can retry more frequently than the old blocking receiver.
A paused player can still leave pending input, so pause is not synonymous with
an empty queue. Power impact has not been measured and no power-neutrality claim
is made. Event-driven idle sleep is outside this change.

## P2: Existing small H.264 hardware-download failure

An offline 1088x368 H.264 animation returns an I/O error through FFmpeg's VA-API
hardware-download path with both the system driver and this candidate. This
case is not counted as a passing regression. Browser-exported buffers are tested
separately. The hardware-download issue is outside the highest-quality HEVC
livestream fix.

Other codec and platform limits remain documented in
[upgrade and pitfalls](../doc/upgrade-and-pitfalls.md). Profile declarations are
unchanged by this contribution.
