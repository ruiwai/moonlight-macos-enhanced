# CS/KZ input on `record`

Ported from `pyrowave/prerequisite-validation` (`b8779e4`, then `3f6cdbf`).
Physical GC, CoreHID and AppKit relative movement goes straight to the serial input
queue instead of waiting for a display tick. Conversion preserves fractional motion;
controller mouse emulation still uses the display link. AppKit provides both wheel
axes, with separate handling for discrete clicks and smooth deltas. Ordinary held
keys track the mapping of the successfully enqueued press, not the current layout.

`moonlight-common/cs-kz-input.patch` adapts Common `77bd732` to `record`'s
`74d0e9f` pin. The worker combines compatible adjacent motion, splits on horizontal
reversals and overflow, and treats keyboard, button and scroll packets as flush
boundaries. At capacity, one of those packets may displace queued relative motion;
a queue full of semantic packets can still reject input. Client enqueue order is
not a guarantee about host delivery or latency.

The PyroWave gitlink predates this branch's controller queues and protocol APIs.
Until there's a published combined Common pin, the Xcode Common target generates
the two patched C sources and two private headers in its derived files directory.
Normal Xcode builds need no manual patch step. The submodule, header layouts and
other translation units remain unchanged. Rebase the patch when updating Common.

With Common and ENet initialized, run `bash tests/input-coherence/run.sh` (CMake,
C11 compiler, Python 3, OpenSSL development headers). On macOS with Homebrew,
use the OpenSSL flags in `.github/workflows/build.yml`. This also checks the Xcode
source generator against a fully patched Common tree. Foundation fixtures require
macOS; portable tests aren't a substitute for capture/recapture and high-polling
mouse tests on a live CS/KZ host.
