# H.264 and HEVC recorder smoke test

Run `tests/h264-recording/run.sh` on macOS with the Xcode Command Line Tools
and both `ffmpeg` (with `libx264` and `libx265`) and `ffprobe` on `PATH`. It
builds `Limelight/Stream/MLCompressedVideoRecorder.m` directly, without building
the app, and provides a local `Log` implementation. All files live in a
temporary directory that the script removes on exit; it does not use `~/Movies`.

For each codec (H.264/`avc1` and HEVC/`hvc1`), the test generates two eight-packet
clips at 96×64 and 128×96. Both codecs use the same two closed GOPs (IDR, P, P, P)
and 25 fps timestamp pattern, including a gap in the second GOP. The harness
runs the same suite separately for each codec, taking the expected codec as an
argument and using separate output directories. It feeds compressed
`AVAssetReader` samples to three recorders:

- **Mid-GOP start:** Skip the first IDR and expect one MOV containing only the
  four packets from the next IDR onward.
- **No IDR:** Feed only the three P frames from the first GOP and expect no file.
- **Independent format and timestamp rollovers:** Feed the last GOP of the
  first clip, then packets 1–7 of the different-resolution clip with PTS (and
  DTS when present) shifted forward by one second after checking the original
  fixture PTS. Replay those same packets without the offset to reset timestamps
  without changing format. Both rollovers must drop the three P frames before
  the next IDR. Expect exactly three readable MOVs in part order (sorted by
  filename, without requiring a specific name): the first clip's last four
  packets, then the second clip's last four packets in each of the next two parts.

Every MOV must have exactly one video track of the specified codec and match
the expected resolution, packet bytes, sync flags, relative PTS/DTS, and
durations. Since `finish` is asynchronous and has no completion callback, the
test polls for verified movies for up to 10 seconds and observes the no-IDR
directory for at least two seconds.

To check the inputs without building the macOS harness, run
`tests/h264-recording/run.sh --fixtures-only`. This runs the same `ffprobe`
check used before the full test: each fixture must have exactly one stream
with the expected codec, sample entry tag, resolution, and eight packets. A
passing probe followed by a reader error distinguishes an AVFoundation read
failure from missing input packets. This mode also runs on Linux; the full
test skips there (exit 77).
