# H.264 recorder smoke test

Run `tests/h264-recording/run.sh` on macOS with the Xcode Command Line Tools
and `ffmpeg` with `libx264` on `PATH`. It builds the real
`Limelight/Stream/MLH264Recorder.m` directly, without building the app, and
provides a local `Log` implementation. All files live in a temporary directory
that the script removes on exit; it does not use `~/Movies`.

The test generates two eight-packet H.264 clips with different resolutions but
the same two closed GOPs and timestamp pattern, including a gap in the second
GOP. It feeds compressed `AVAssetReader` samples to three recorders:

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

Every MOV must have exactly one H.264 video track and match the expected
resolution, packet bytes, sync flags, relative PTS/DTS, and durations. Since
`finish` is asynchronous and has no completion callback, the test polls for
verified movies for up to 10 seconds and observes the no-IDR directory for at
least two seconds.

On other operating systems the runner reports a skip (exit 77).
