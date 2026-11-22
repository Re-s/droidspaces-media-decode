# Browser frame-order verification

These tests exercise Chrome's actual presentation path. The
[FFmpeg contract regression](../regress_chrome_contract.sh) can simulate
early surface sampling with `DMD_NO_MAP_WAIT=1`. The browser pages also cover
Media Source Extensions (MSE) chunk delivery, changes to `src`, and the
browser's choice of which decoded surface to present.

## What to measure

`requestVideoFrameCallback` reports `mediaTime`, which the test converts
to an expected frame index. The MSE page also makes a canvas pixel signature.
Compare it with a software-decoded reference to identify which frame the
browser actually presented. A correct timestamp can accompany stale pixels.

Callbacks can miss presented frames. In one comparison, build `9f00b1a6`
and a fixed build produced identical, increasing frame-index sequences
despite different pixels. Use the canvas signature for pixel validation.

Check the timestamp origin of the fixture before comparing frame indices.
In the tested fragmented MP4 generated with
`-movflags frag_keyframe+empty_moov`, `start_time` was 1.0 seconds versus
0.0 in the source. At 2 fps, that introduced a constant two-frame offset.

## Run the MSE test

From this directory, stage the test pages and a compatible H.264 fragmented
MP4 in a working directory. Start the report server with a fresh output path.

```sh
test_dir=$(mktemp -d)
cp mse_test.html net_test.html abr_test.html "$test_dir/"
cp /path/to/h264_frag.mp4 "$test_dir/"
OUT="$test_dir/report.json" ROOT="$test_dir" WANT_REPORTS=1 python3 report.py 8871
```

In the desktop user's Wayland session, open the page with the
[configured VA-API Chrome launcher](../../../README.md#chrome-on-gnome-wayland).
Set the MIME codec and frame rate to match the fixture. This example uses
H.264 High at 2 fps and seeks once halfway through the test.

```sh
"$HOME/.local/bin/chrome-msm-vaapi" --autoplay-policy=no-user-gesture-required \
  'http://127.0.0.1:8871/mse_test.html?src=h264_frag.mp4&mime=video/mp4;%20codecs%3D%22avc1.64001f%22&fps=2&n=20&seek=1'
```

Confirm the decoder in `chrome://media-internals` and inspect
`report.json` in the working directory. Stop the report server after it
writes the result if it is still running.

| Page | Playback path | Parameters |
|---|---|---|
| `mse_test.html` | MSE chunk delivery with canvas pixel signatures | `src`, `mime`, `fps`, `n` samples, `chunk` bytes, `seek=1` |
| `net_test.html` | Direct HTTP video and frame-index sampling | `url`, `fps`, `n`, `seek=1`. Cross-origin fixtures must allow CORS |
| `abr_test.html` | Resolution changes by switching `src` | Set the in-page `srcs` list and the `PER` frames-per-source constant |

## Recorded results and limitations

- Direct HEVC playback of the 1280×720 `jinjie_265.mp4` fixture produced
  24 monotonic samples and 231 driver frame matches. The seek run sampled
  frames 0–11 and 86–97 without backward samples or reported drops.
- The 640×360 → 1280×720 → 640×360 source switch produced ten samples per
  source. All three runs followed the expected resolution with no backward
  samples. The driver opened three sessions and logged 86 frame matches.
- The tested Chrome build rejected the HEVC MSE fixture with `sb error`
  and `InvalidStateError`, while direct `<video src>` playback yielded
  131 hardware frame matches. Use direct playback for that fixture and
  an offline software decoder for reference pixels. Use H.264 when the
  browser needs to provide both the hardware and software paths.

## Test page timing

Changing `src` cancels pending frame callbacks. Register a fresh callback
after `loadeddata` so it uses the new media state. The original test without
that wait stalled after opening one decode session.

MSE `updateend` can finish before the video reaches `HAVE_CURRENT_DATA`.
The MSE test also starts playback on `loadeddata` and `canplay` and retries
on a timer. Without those fallbacks, a fast append could produce just one
sample before the timeout.
