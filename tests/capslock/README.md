# Caps Lock regression checks

Run `python3 tests/capslock/run.py` on macOS with Xcode or Command Line Tools.

The tests execute the production AppKit event normalizer and Caps Lock routing
branch with isolated output. They cover lock-on and lock-off transitions,
duplicate edges, keyboard capture, recapture, and unrelated modifiers. Recorder
wiring and the supported display label are checked separately. No events are
posted to macOS or a remote host.

A Release build passed with Xcode 27.1 on Apple Silicon. A user verified that
firmware-layer Fn+Tab mapped to standard Caps Lock works in a real streaming
session after this fix. Intel hardware has not been tested.
