# Jellyfin rotation patches

Use the metadata-preserving path when the playback client handles MP4 display
rotation. Do not apply the legacy transpose patch together with it.

- `jellyfin-v12.2-vaapi-rotation-metadata.patch` changes Jellyfin 12.2 to leave
  VAAPI-decoded frames in their source orientation and request a `rotate` track
  metadata value from the MP4/fMP4 muxer.
- `jellyfin-ffmpeg8-movenc-rotate-metadata.patch` is needed for Jellyfin FFmpeg
  8. Its MOV muxer currently ignores the conventional `rotate` stream metadata
  on transcoded streams; the patch uses that value as a display-matrix fallback.
- `jellyfin-v12.2-msm-drm-transpose-vaapi-legacy.patch` is the earlier pixel
  transpose approach. It physically rotates frames and is an alternative to
  the metadata path above.

Both metadata patches target Jellyfin 12.2 and Jellyfin FFmpeg 8 source
respectively. The Jellyfin change alone works with muxers that already convert
`rotate` stream metadata into a display matrix (for example, the tested stock
FFmpeg 6 MP4/fMP4 muxer).

For variable-frame-rate inputs whose `r_frame_rate` incorrectly reports the MP4
90 kHz timebase, use `jellyfin-v12.2-hls-vfr-timestamps.patch`. It selects
passthrough frame sync for encoded HLS output and uses Jellyfin's
`ReferenceFrameRate` for HLS playlist and keyframe interval calculations.
