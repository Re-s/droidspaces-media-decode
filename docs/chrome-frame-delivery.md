# Chrome frame delivery

Chrome HEVC playback can stall when a background receive owns the same session
I/O needed to submit more input. Pending input does not imply ready output:
a stateful decoder may require later input before releasing a reordered picture.

The previous background receive could hold this ownership for about 20 ms.
An exported target could also be recycled while its old submission remained
pending; waiting for that old output before sending the new input could produce
repeated 5000 ms waits.

## Receive ownership

```mermaid
flowchart LR
    Background[Background receiver] --> Try[Try currently available output]
    Synchronization[Explicit synchronization] --> Wait[Receive with a timeout]
    Try --> Receive[Shared queue-first receive]
    Wait --> Receive
    Receive --> Publish[Publish geometry and input-unit identity]
    Publish --> Release[Return the borrowed CAPTURE buffer]
    Release --> Unlock[Release session I/O ownership]
```

`dmd_session_try_next_frame()` checks retained frames first, then calls the existing
backend with a zero poll timeout. An empty result is `DMD_ERR_TIMEOUT`, not a
fatal session error. It still processes format events.

`dmd_session_next_frame()` retains its existing explicit-wait behavior, including
using the configured timeout for nonpositive arguments. Both entry points share
queue order, format publication, frame construction and error handling.

The background receiver retains ownership through the last session operation,
including `release_frame`, so deferred destruction cannot close a borrowed
session. It yields outside ownership when it has delivered nothing. The thread
is retained to deliver completed output even without another submission.

## Recycled targets and capacity

For exported H.264/HEVC targets, a recycled target's older pending entries retain
their input-unit identity but have a discard target. Late output still removes
exactly its matching record and returns its CAPTURE buffer. It cannot overwrite
the recycled target or fall through to an ordering guess. Non-exported reads
retain their synchronous reuse behavior.

The display-wide surface object table is separate from a context's 64-entry
pending queue. Multiple decoder pools can coexist on a display without sharing
one context's object budget. Pixel buffers are still allocated on demand.

## Validation

The fix was exercised on an iQOO Pad6 Pro with Droidspaces 7.2.0, Ubuntu 26.04
arm64, Chrome 157 and an Android 16 vendor kernel. Software decoding was used
only to produce offline reference hashes; browser playback remained hardware
decoded at the selected highest quality.

- `make all`, `make check` and `make tests` pass. Receiver tests cover an empty
  nonblocking attempt, FIFO order and wraparound, a final ready frame without
  new input, format changes, release failure, repeated release, EOS, invalid
  arguments and preserved explicit-wait semantics.
- All 300 HEVC hardware frames match reference pixels and timestamps.
- Browser buffer samples match the reference for 29 HEVC and 13 H.264 frames.
- All 25 retired input records in the focused loop/seek test are consumed by
  exact identity, with no unmatched records or 5000 ms waits.
- A hardware VA-API probe creates 70 surfaces, queries object 69 and destroys
  them successfully. The old display-wide 64-slot table rejects that request.
- Background receive samples: 1718 calls, median 0.006 ms, p95 0.014 ms,
  maximum 1.645 ms. The old blocking receive had median 20.087 ms and p95 21.099 ms.
- Highest-quality 2560x1440 HEVC playback ran for 390.689 seconds: 23416 new
  frames, 18 reported drops, about 0.077 percent. The user reported smooth
  playback and requested monitoring to stop; this was not a ten-minute test.

`requestVideoFrameCallback` can miss callbacks. Its `presentedFrames` metadata
counts frames submitted for composition, so callback count alone is not a display
frame-rate measurement. In a 25-second sample, 1409 callbacks covered 1500 frames
submitted for composition and 25 seconds of media time. See the
[API proposal](https://wicg.github.io/video-rvfc/).

See [known limitations](known-limitations.md) for power and stream-end boundaries.
