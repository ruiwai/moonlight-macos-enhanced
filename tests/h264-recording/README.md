# H.264 recorder smoke test

Run `tests/h264-recording/run.sh` on macOS with the Xcode Command Line Tools
and `ffmpeg` with `libx264` on `PATH`. It builds the real
`Limelight/Stream/MLH264Recorder.m` directly, without building the app, and
provides a local `Log` implementation. All files live in a temporary directory
that the script removes on exit; it does not use `~/Movies`.

The test generates an eight-packet H.264 clip and skips its first IDR to simulate
joining mid-GOP. It feeds compressed `AVAssetReader` samples to the recorder and
verifies that the resulting `.mov` contains exactly one H.264 video track and
starts at the next IDR, with the expected packet bytes, sync flags, relative
PTS/DTS, and durations. A second recorder sees only the three non-IDR packets
and must leave its directory empty. Since `finish` is asynchronous and has no
completion callback, the test polls for a readable, verified movie for up to
10 seconds and observes the no-IDR directory for at least two seconds.

On other operating systems the runner reports a skip (exit 77).
